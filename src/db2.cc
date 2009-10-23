/*
  db2.cc

  DB2 OCI Interface to Qore DBI layer

  Qore Programming Language

  Copyright 2009 Qore Technologies, sro

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "../config.h"
#include <qore/Qore.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>

#include <sqlcli1.h>
#include <sqlutil.h>
#include <sqlenv.h>

#include <memory>
#include <string>

static QoreStringNode *db2_module_init();
static void db2_module_ns_init(QoreNamespace *rns, QoreNamespace *qns);
static void db2_module_delete();

DLLEXPORT char qore_module_name[] = "db2";
DLLEXPORT char qore_module_version[] = PACKAGE_VERSION;
DLLEXPORT char qore_module_description[] = "DB2 database driver";
DLLEXPORT char qore_module_author[] = "Qore Technologies, sro";
DLLEXPORT char qore_module_url[] = "http://www.qoretechnologies.com";
DLLEXPORT int qore_module_api_major = QORE_MODULE_API_MAJOR;
DLLEXPORT int qore_module_api_minor = QORE_MODULE_API_MINOR;
DLLEXPORT qore_module_init_t qore_module_init = db2_module_init;
DLLEXPORT qore_module_ns_init_t qore_module_ns_init = db2_module_ns_init;
DLLEXPORT qore_module_delete_t qore_module_delete = db2_module_delete;
DLLEXPORT qore_license_t qore_module_license = QL_LGPL;

// capabilities of this driver
#define DBI_DB2_CAPS (DBI_CAP_TRANSACTION_MANAGEMENT | DBI_CAP_STORED_PROCEDURES | DBI_CAP_CHARSET_SUPPORT | DBI_CAP_LOB_SUPPORT | DBI_CAP_BIND_BY_VALUE | DBI_CAP_BIND_BY_PLACEHOLDER)

DBIDriver *DBID_DB2 = 0;

static QoreString QoreDB2ClientName;
static QoreString this_hostname;

class QoreDB2Handle {
};

class QoreDB2 {
private:
   SQLHANDLE henv, hdbc;

   // add diagnostic info to exception description string
   DLLLOCAL static void addDiagnostics(SQLSMALLINT htype, SQLHANDLE hndl, QoreStringNode *desc) {
      SQLCHAR message[SQL_MAX_MESSAGE_LENGTH + 1];
      SQLCHAR sqlstate[SQL_SQLSTATE_SIZE + 1];
      SQLINTEGER sqlcode;
      SQLSMALLINT length, i = 1;

      // get multiple field settings of diagnostic record
      while (SQLGetDiagRec(htype, hndl, i, sqlstate, &sqlcode, message, SQL_MAX_MESSAGE_LENGTH + 1, &length) == SQL_SUCCESS) {
	 if (i > 1)
	    desc->concat("; ");
	 desc->sprintf("SQLSTATE: %s, native error code: %d: %s", sqlstate, sqlcode, message);
	 i++;
      }
      if (i == 1)
	 desc->concat("no diagnostic information available");
   }

public:
   DLLLOCAL QoreDB2(const std::string &dbname, const std::string &user, const std::string &pass, ExceptionSink *xsink) : henv(0), hdbc(0) {
      SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
      if (rc != SQL_SUCCESS) {
	 xsink->raiseException("DBI:DB2:OPEN-ERROR", "error allocating environment handle: SQLAllocHandle() returned %d", rc);
	 return;
      }

      // set environment attributes
      rc = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void *)SQL_OV_ODBC3, 0);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open() SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION)", xsink))
	 return;

      rc = SQLSetEnvAttr(henv, SQL_ATTR_INFO_APPLNAME, (void *)QoreDB2ClientName.getBuffer(), 0);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open() SQLSetEnvAttr(SQL_ATTR_INFO_APPLNAME)", xsink))
	 return;

      rc = SQLSetEnvAttr(henv, SQL_ATTR_INFO_WRKSTNNAME, (void *)this_hostname.getBuffer(), 0);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open() SQLSetEnvAttr(SQL_ATTR_INFO_WRKSTNNAME)", xsink))
	 return;

      rc = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open() SQLAllocHandle(SQL_HANDLE_DBC)", xsink))
	 return;

      // set connection attributes
      rc = SQLSetConnectAttr(hdbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, SQL_NTS);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open() SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT)", xsink))
	 return;

      rc = SQLConnect(hdbc, (SQLCHAR *)dbname.c_str(), dbname.size(), (SQLCHAR *)user.c_str(), user.size(), (SQLCHAR *)pass.c_str(), pass.size());
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open() SQLConnect()", xsink))
	 return;
   }

   DLLLOCAL ~QoreDB2() {
      if (hdbc) {
	 // TODO: check return value?
	 SQLDisconnect(hdbc);
	 SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
      }

      if (henv) {
	 SQLFreeHandle(SQL_HANDLE_ENV, henv);
      }
   }

   DLLLOCAL static int checkError(SQLSMALLINT htype, SQLHANDLE hndl, SQLRETURN rc, const char *info, ExceptionSink *xsink) {
      switch (rc) {
	 case SQL_SUCCESS:
	    return 0;

	 case SQL_INVALID_HANDLE:
	    assert(false);
	    return -1;

	 case SQL_ERROR: {
	    QoreStringNode *desc = new QoreStringNode();
	    desc->sprintf("error %d in %s: ", rc, info);
	    addDiagnostics(htype, hndl, desc);
	    xsink->raiseException("DBI:DB2:ERROR", desc);
	    return -1;
	 }

	 case SQL_SUCCESS_WITH_INFO:
	 case SQL_STILL_EXECUTING:
	 case SQL_NEED_DATA:
	 case SQL_NO_DATA_FOUND:
	    return 0;
      }

      QoreStringNode *desc = new QoreStringNode();
      desc->sprintf("unknown error  %d returned in %s: ", rc, info);
      addDiagnostics(htype, hndl, desc);
      xsink->raiseException("DBI:DB2:ERROR", desc);
      return -1;
   }
};

static AbstractQoreNode *db2_exec(Datasource *ds, const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink) {
   return 0;
}

static AbstractQoreNode *db2_select(Datasource *ds, const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink) {
   return 0;
}

static AbstractQoreNode *db2_select_rows(Datasource *ds, const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink) {
   return 0;
}

static int db2_commit(class Datasource *ds, ExceptionSink *xsink) {
   return 0;
}

static int db2_rollback(class Datasource *ds, ExceptionSink *xsink) {
   return 0;
}

static int db2_open(Datasource *ds, ExceptionSink *xsink) {
   if (!ds->getDBName()) {
      xsink->raiseException("DATASOURCE-MISSING-DBNAME", "missing dbname in datasource connection");
      return -1;
   }

   std::auto_ptr<QoreDB2> db2(new QoreDB2(ds->getDBNameStr(), ds->getUsernameStr(), ds->getPasswordStr(), xsink));
   if (*xsink)
      return -1;
   
   ds->setPrivateData((void *)db2.release());
   return 0;
}

static int db2_close(Datasource *ds) {
   QORE_TRACE("db2_close()");

   QoreDB2 *d_db2 = (QoreDB2 *)ds->getPrivateData();
   delete d_db2;

   ds->setPrivateData(0);
   return 0;
}

static AbstractQoreNode *db2_get_server_version(Datasource *ds, ExceptionSink *xsink) {
   // get private data structure for connection
   //QoreDB2 *db2 = (QoreDB2 *)ds->getPrivateData();

   return 0;
}

static AbstractQoreNode *db2_get_client_version(const Datasource *ds, ExceptionSink *xsink) {
   return 0;
}

#ifndef HOSTNAMEBUFSIZE
#define HOSTNAMEBUFSIZE 512
#endif

static QoreStringNode *db2_module_init() {
   QORE_TRACE("db2_module_init()");

   QoreDB2ClientName.sprintf("Qore DB2 driver v%s on Qore v%s %s %s", PACKAGE_VERSION, qore_version_string, qore_target_arch, qore_target_os);

   // get hostname
   char buf[HOSTNAMEBUFSIZE + 1];
   this_hostname.set(!gethostname(buf, HOSTNAMEBUFSIZE) ? buf : "<unknown>");

   // register driver with DBI subsystem
   qore_dbi_method_list methods;
   methods.add(QDBI_METHOD_OPEN, db2_open);
   methods.add(QDBI_METHOD_CLOSE, db2_close);
   methods.add(QDBI_METHOD_SELECT, db2_select);
   methods.add(QDBI_METHOD_SELECT_ROWS, db2_select_rows);
   methods.add(QDBI_METHOD_EXEC, db2_exec);
   methods.add(QDBI_METHOD_COMMIT, db2_commit);
   methods.add(QDBI_METHOD_ROLLBACK, db2_rollback);
   methods.add(QDBI_METHOD_GET_SERVER_VERSION, db2_get_server_version);
   methods.add(QDBI_METHOD_GET_CLIENT_VERSION, db2_get_client_version);
   
   DBID_DB2 = DBI.registerDriver("db2", methods, DBI_DB2_CAPS);

   return 0;
}

static void db2_module_ns_init(QoreNamespace *rns, QoreNamespace *qns) {
   QORE_TRACE("db2_module_ns_init()");
   // nothing to do at the moment
}

static void db2_module_delete() {
   QORE_TRACE("db2_module_delete()");
}
