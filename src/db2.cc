/*
  db2.cc

  DB2 OCI Interface to Qore DBI layer

  Qore Programming Language

  Copyright 2009 - 2010 Qore Technologies, sro

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
#include <map>

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

#ifdef _QORE_HAS_DBI_EXECRAW
#define DB2_DBI_CAP_HAS_EXECRAW DBI_CAP_HAS_EXECRAW
#else
#define DB2_DBI_CAP_HAS_EXECRAW 0
#endif
// capabilities of this driver
#define DBI_DB2_CAPS (DBI_CAP_TRANSACTION_MANAGEMENT | DBI_CAP_STORED_PROCEDURES | DBI_CAP_CHARSET_SUPPORT | DBI_CAP_LOB_SUPPORT | DBI_CAP_BIND_BY_VALUE | DBI_CAP_BIND_BY_PLACEHOLDER | DB2_DBI_CAP_HAS_EXECRAW )

DBIDriver *DBID_DB2 = 0;

static QoreString QoreDB2ClientName;
static QoreString this_hostname;
static const char *this_user = 0;

#define QORE_DB2_MAX_COL_NAME_LEN 128

//class QoreDB2Handle {};

// type for mapping DB2 code pages to character encodings
typedef std::map<int, const char *> qore_db2_cp_map_t;
// maps DB2 code pages to character encodings
qore_db2_cp_map_t qore_db2_cp_map;

class QoreDB2Column {
protected:
   int col_no;
   SQLSMALLINT colType;
   SQLUINTEGER colSize;
   SQLSMALLINT colScale;
   SQLINTEGER ind;
   std::string cname;
   union bind_buf_u {
      char *cstr;
      int64 bigint;
      double c_double;
      DATE_STRUCT date;
      TIME_STRUCT time;
      TIMESTAMP_STRUCT *timestamp;
   } buf;
   QoreListNode *l;

public:
   DLLLOCAL QoreDB2Column() : l(0) {}
   DLLLOCAL ~QoreDB2Column();
   DLLLOCAL int describeAndBind(SQLHANDLE hstmt, int col_no, char *cnbuf, int cnbufsize, ExceptionSink *xsink);

   DLLLOCAL const char *getName() const { return cname.c_str(); }
   DLLLOCAL SQLSMALLINT getType() const { return colType; }
   DLLLOCAL SQLSMALLINT getSize() const { return colSize; }
   DLLLOCAL AbstractQoreNode *getValue(const QoreEncoding *enc, ExceptionSink *xsink) const;
   DLLLOCAL int doValue(const QoreEncoding *enc, ExceptionSink *xsink) {
      if (!l)
	 l = new QoreListNode;
      l->push(getValue(enc, xsink));
      return *xsink ? -1 : 0;
   }
   DLLLOCAL QoreListNode *takeList() {
      QoreListNode *rv = l;
      l = 0;
      return rv;
   }
   DLLLOCAL qore_size_t size() const {
      return l ? l->size() : 0;
   }
};

class QoreDB2;

class QoreDB2Result {
protected:
   SQLSMALLINT cols;
   QoreDB2 *qdb2;
   SQLHANDLE hstmt;
   QoreDB2Column *columns;

public:
   DLLLOCAL QoreDB2Result(QoreDB2 *qdb2, const QoreString &sql, const QoreListNode *args, ExceptionSink *xsink);
   DLLLOCAL ~QoreDB2Result() {
      if (columns)
	 delete [] columns;
      if (hstmt)
	 SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
   }
   DLLLOCAL QoreHashNode *getHash(const QoreEncoding *enc, ExceptionSink *xsink);
   DLLLOCAL QoreListNode *getList(const QoreEncoding *enc, ExceptionSink *xsink);
};

class QoreDB2 {
private:
   SQLHANDLE henv, hdbc;
   Datasource *ds;

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
      desc->trim();
   }

public:
   DLLLOCAL QoreDB2(Datasource &n_ds, ExceptionSink *xsink) : henv(0), hdbc(0), ds(&n_ds) {
      SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);
      if (rc != SQL_SUCCESS) {
	 xsink->raiseException("DBI:DB2:OPEN-ERROR", "error allocating environment handle: SQLAllocHandle() returned %d", rc);
	 return;
      }

      //printd(5, "QoreDB2::QoreDB2() calling SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION)\n");
      // set environment attributes
      rc = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (void *)SQL_OV_ODBC3, 0);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open: SQLSetEnvAttr(SQL_ATTR_ODBC_VERSION)", xsink))
	 return;

      //printd(5, "QoreDB2::QoreDB2() calling SQLSetEnvAttr(SQL_ATTR_INFO_APPLNAME)\n");
      rc = SQLSetEnvAttr(henv, SQL_ATTR_INFO_APPLNAME, (void *)QoreDB2ClientName.getBuffer(), 0);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open: SQLSetEnvAttr(SQL_ATTR_INFO_APPLNAME)", xsink))
	 return;

      //printd(5, "QoreDB2::QoreDB2() calling SQLSetEnvAttr(SQL_ATTR_INFO_WRKSTNNAME)\n");
      rc = SQLSetEnvAttr(henv, SQL_ATTR_INFO_WRKSTNNAME, (void *)this_hostname.getBuffer(), 0);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open: SQLSetEnvAttr(SQL_ATTR_INFO_WRKSTNNAME)", xsink))
	 return;

      //printd(5, "QoreDB2::QoreDB2() calling SQLSetEnvAttr(SQL_ATTR_INFO_USERID)\n");
      rc = SQLSetEnvAttr(henv, SQL_ATTR_INFO_USERID, (void *)this_user, 0);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open: SQLSetEnvAttr(SQL_ATTR_INFO_USERID)", xsink))
	 return;

      //printd(5, "QoreDB2::QoreDB2() calling SQLAllocHandle(SQL_HANDLE_DBC)\n");
      rc = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open: SQLAllocHandle(SQL_HANDLE_DBC)", xsink))
	 return;

      // set connection attributes
      //printd(5, "QoreDB2::QoreDB2() calling SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF)\n");
      rc = SQLSetConnectAttr(hdbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, SQL_NTS);
      if (QoreDB2::checkError(SQL_HANDLE_ENV, henv, rc, "open: SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF)", xsink))
	 return;

      // build connection string
      QoreString str;
      if (n_ds.getHostName() && n_ds.getPort())
	 str.sprintf("database=%s;hostname=%s;servicename=%d;protocol=tcpip", n_ds.getDBName(), n_ds.getHostName(), n_ds.getPort());
      else
	 str.sprintf("dsn=%s", n_ds.getDBName());
      if (n_ds.getUsername())
	 str.sprintf(";uid=%s", n_ds.getUsername());
      if (n_ds.getPassword())
	 str.sprintf(";pwd=%s", n_ds.getPassword());

      //printd(0, "QoreDB2::QoreDB2() connection string: '%s'\n", str.getBuffer());

      rc = SQLDriverConnect(hdbc, 0, (SQLCHAR *)str.getBuffer(), str.strlen(), 0, 0, 0, SQL_DRIVER_NOPROMPT);
      if (QoreDB2::checkError(SQL_HANDLE_DBC, hdbc, rc, "open: SQLDriverConnect()", xsink))
	 return;

      // get connection code page
      SQLINTEGER codepage;
      SQLSMALLINT len;
      rc = SQLGetInfo(hdbc, SQL_CONNECT_CODEPAGE, &codepage, sizeof(SQLINTEGER), &len);
      if (QoreDB2::checkError(SQL_HANDLE_DBC, hdbc, rc, "open: SQLGetInfo(SQL_CONNECT_CODEPAGE)", xsink))
	 return;

      const QoreEncoding *enc;
      qore_db2_cp_map_t::iterator i = qore_db2_cp_map.find(codepage);
      str.clear();
      if (i == qore_db2_cp_map.end()) {
	 str.sprintf("ibm-%d", codepage);
	 enc = QEM.findCreate(&str);
	 str.clear();
      }
      else
	 enc = QEM.findCreate(i->second);
      str.sprintf("%d", codepage);

      // set character encoding for connection
      n_ds.setQoreEncoding(enc);
      n_ds.setDBEncoding(str.getBuffer());
      //printd(0, "connection codepage is %d: %s\n", codepage, enc->getCode());
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

   DLLLOCAL int commit(ExceptionSink *xsink) {
      SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_COMMIT);
      if (QoreDB2::checkError(SQL_HANDLE_DBC, hdbc, rc, "SQLEndTran(SQL_COMMIT)", xsink))
	 return -1;
      return 0;
   }

   DLLLOCAL int rollback(ExceptionSink *xsink) {
      SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, hdbc, SQL_ROLLBACK);
      if (QoreDB2::checkError(SQL_HANDLE_DBC, hdbc, rc, "SQLEndTran(SQL_ROLLBACK)", xsink))
	 return -1;
      return 0;
   }

   DLLLOCAL AbstractQoreNode *select(const QoreString &sql, const QoreListNode *args, ExceptionSink *xsink) {
      QoreDB2Result res(this, sql, args, xsink);
      if (*xsink)
	 return 0;

      return res.getHash(ds->getQoreEncoding(), xsink);
   }

   DLLLOCAL AbstractQoreNode *exec(const QoreString &sql, const QoreListNode *args, ExceptionSink *xsink) {
      return 0;
   }

#ifdef _QORE_HAS_DBI_EXECRAW
   DLLLOCAL AbstractQoreNode *execRaw(const QoreString &sql, ExceptionSink *xsink) {
      return 0;
   }
#endif

   DLLLOCAL AbstractQoreNode *select_rows(const QoreString &sql, const QoreListNode *args, ExceptionSink *xsink) {
      QoreDB2Result res(this, sql, args, xsink);
      if (*xsink)
	 return 0;

      return res.getList(ds->getQoreEncoding(), xsink);
   }

   DLLLOCAL SQLHANDLE getDBCHandle() {
      return hdbc;
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

QoreDB2Result::QoreDB2Result(QoreDB2 *n_qdb2, const QoreString &sql, const QoreListNode *args, ExceptionSink *xsink) : qdb2(n_qdb2), hstmt(0), columns(0) {
   SQLRETURN rc = SQLAllocHandle(SQL_HANDLE_STMT, qdb2->getDBCHandle(), &hstmt);
   if (QoreDB2::checkError(SQL_HANDLE_DBC, qdb2->getDBCHandle(), rc, "QoreDB2Result::QoreDB2Result(): SQLAllocHandle(SQL_HANDLE_STMT)", xsink))
      return;

   // prepare the statement
   rc = SQLPrepare(hstmt, (SQLCHAR*)sql.getBuffer(), SQL_NTS);
   if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "QoreDB2Result::QoreDB2Result(): SQLPrepare()", xsink))
      return;

   rc = SQLExecute(hstmt);
   if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "QoreDB2Result::QoreDB2Result(): SQLExecute()", xsink))
      return;

   rc = SQLNumResultCols(hstmt, &cols);
   if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "QoreDB2Result::QoreDB2Result(): SQLNumResultCols()", xsink))
      return;

   printd(5, "QoreDB2::select() query returned %d columns\n", (int)cols);

   if (!cols)
      return;

   // column name buffer
   char cname[QORE_DB2_MAX_COL_NAME_LEN + 1];
   columns = new QoreDB2Column[cols];

   // bind all columns
   for (int i = 0; i < cols; ++i) {
      if (columns[i].describeAndBind(hstmt, i, cname, QORE_DB2_MAX_COL_NAME_LEN + 1, xsink))
	 return;
   }
}

QoreHashNode *QoreDB2Result::getHash(const QoreEncoding *enc, ExceptionSink *xsink) {
   while (true) {
      SQLRETURN rc = SQLFetch(hstmt);
      if (rc == SQL_NO_DATA_FOUND)
	 break;

      for (int i = 0; i < cols; ++i) {
	 if (columns[i].doValue(enc, xsink))
	    return 0;
      }
   }

   // return hash of lists
   QoreHashNode *h = new QoreHashNode();
   for (int i = 0; i < cols; ++i)
      h->setKeyValue(columns[i].getName(), columns[i].takeList(), xsink);

   return h;
}

QoreListNode *QoreDB2Result::getList(const QoreEncoding *enc, ExceptionSink *xsink) {
   ReferenceHolder<QoreListNode> l(new QoreListNode, xsink);

   while (true) {
      SQLRETURN rc = SQLFetch(hstmt);
      if (rc == SQL_NO_DATA_FOUND)
	 break;

      ReferenceHolder<QoreHashNode> h(new QoreHashNode(), xsink);
      for (int i = 0; i < cols; ++i) {
	 AbstractQoreNode *v = columns[i].getValue(enc, xsink);
	 if (*xsink) {
	    assert(!v);
	    return 0;
	 }
	 h->setKeyValue(columns[i].getName(), v, xsink);
	 if (*xsink)
	    return 0;
      }

      l->push(h.release());
   }

   return l.release();
}

AbstractQoreNode *QoreDB2Column::getValue(const QoreEncoding *enc, ExceptionSink *xsink) const {
   if (ind == SQL_NULL_DATA)
      return null();

   switch (colType) {
      case SQL_TINYINT:
      case SQL_C_LONG:
      case SQL_C_SHORT:
      case SQL_BIGINT:
	 return new QoreBigIntNode(buf.bigint);

      case SQL_FLOAT:
      case SQL_REAL:
      case SQL_DOUBLE:
	 return new QoreFloatNode(buf.c_double);

      case SQL_TYPE_DATE:
	 return new DateTimeNode(buf.date.year, buf.date.month, buf.date.day);

      case SQL_TYPE_TIME:
	 return new DateTimeNode(1970, 1, 1, buf.time.hour, buf.time.minute, buf.time.second);

      case SQL_TYPE_TIMESTAMP:
#ifdef _QORE_HAS_TIME_ZONES
	 return DateTimeNode::makeAbsolute(currentTZ(), 
					   buf.timestamp->year, buf.timestamp->month, buf.timestamp->day,
					   buf.timestamp->hour, buf.timestamp->minute, buf.timestamp->second,
					   buf.timestamp->fraction / 1000);
#else
	 return new DateTimeNode(buf.timestamp->year, buf.timestamp->month, buf.timestamp->day,
				 buf.timestamp->hour, buf.timestamp->minute, buf.timestamp->second,
				 buf.timestamp->fraction / 1000000);
#endif
      case SQL_BIT:
      case SQL_BINARY:
      case SQL_VARBINARY: {
	 //printd(5, "creating binary %p len=%d\n", buf.cstr, ind);
	 BinaryNode *b = new BinaryNode;
	 b->append(buf.cstr, ind);
	 return b;
      }
   }

   // default: handle as string
   // FIXME: set encoding
   return new QoreStringNode(buf.cstr, enc);
}

int QoreDB2Column::describeAndBind(SQLHANDLE hstmt, int col_no, char *cnbuf, int cnbufsize, ExceptionSink *xsink) {
   SQLSMALLINT colNameLen;
   SQLRETURN rc = SQLDescribeCol(hstmt, (SQLSMALLINT)(col_no + 1), (SQLCHAR*)cnbuf, cnbufsize, &colNameLen, &colType, &colSize, &colScale, 0);
   if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "select: SQLDescribeCol()", xsink))
      return -1;

   // make column name lower case
   strtolower(cnbuf);

   // save a copy of the column name
   cname = cnbuf;

   switch (colType) {
      case SQL_TINYINT:
      case SQL_C_LONG:
      case SQL_C_SHORT:
      case SQL_BIGINT:
	 rc = SQLBindCol(hstmt, col_no + 1, SQL_C_SBIGINT, &buf.bigint, sizeof(buf.bigint), &ind);
	 if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "select: SQLBindCol()", xsink))
	    return -1;
	 break;

      case SQL_FLOAT:
      case SQL_REAL:
      case SQL_DOUBLE:
	 rc = SQLBindCol(hstmt, col_no + 1, SQL_C_DOUBLE, &buf.c_double, sizeof(buf.c_double), &ind);
	 if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "select: SQLBindCol()", xsink))
	    return -1;
	 break;

      case SQL_TYPE_DATE:
	 rc = SQLBindCol(hstmt, col_no + 1, SQL_C_TYPE_DATE, &buf.date, sizeof(buf.date), &ind);
	 if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "select: SQLBindCol()", xsink))
	    return -1;
	 break;

      case SQL_TYPE_TIME:
	 rc = SQLBindCol(hstmt, col_no + 1, SQL_C_TYPE_TIME, &buf.time, sizeof(buf.time), &ind);
	 if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "select: SQLBindCol()", xsink))
	    return -1;
	 break;

      case SQL_TYPE_TIMESTAMP: {
	 buf.timestamp = (TIMESTAMP_STRUCT *)malloc(sizeof(TIMESTAMP_STRUCT));
	 rc = SQLBindCol(hstmt, col_no + 1, SQL_C_TYPE_TIMESTAMP, buf.timestamp, sizeof(TIMESTAMP_STRUCT), &ind);
	 if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "select: SQLBindCol()", xsink))
	    return -1;
	 break;
      }

      case SQL_BIT:
      case SQL_VARBINARY:
      case SQL_BINARY: {
	 buf.cstr = (char *)malloc(sizeof(char) * (colSize));
	 rc = SQLBindCol(hstmt, col_no + 1, SQL_C_BINARY, buf.cstr, colSize, &ind);
	 if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "select: SQLBindCol()", xsink))
	    return -1;
	 break;
      }

      // default: handle as string
      default: {
	 buf.cstr = (char *)malloc(sizeof(char) * (colSize + 1));
	 rc = SQLBindCol(hstmt, col_no + 1, SQL_C_CHAR, buf.cstr, colSize + 1, &ind);
	 if (QoreDB2::checkError(SQL_HANDLE_STMT, hstmt, rc, "select: SQLBindCol()", xsink))
	    return -1;
	 break;
      }
   }

   printd(5, "QoreDB2Column::describeAndBind() col %d: %s type %d size %d\n", col_no, cnbuf, colType, colSize);
   return 0;
}

QoreDB2Column::~QoreDB2Column() {
   // we have data to delete
   if (!cname.empty()) {
      switch (colType) {
	 case SQL_TINYINT:
	 case SQL_C_LONG:
	 case SQL_C_SHORT:
	 case SQL_BIGINT:
	 case SQL_DOUBLE:
	 case SQL_FLOAT:
	 case SQL_REAL:
	 case SQL_TYPE_DATE:
	 case SQL_TYPE_TIME:
	    break;

	 case SQL_TYPE_TIMESTAMP:
	    free(buf.timestamp);
	    break;

	 // default: string data
	 default:
	    if (buf.cstr)
	       free(buf.cstr);
	    break;
      }
   }
   if (l)
      l->deref(0);
}

static AbstractQoreNode *db2_exec(Datasource *ds, const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink) {
   return reinterpret_cast<QoreDB2 *>(ds->getPrivateData())->exec(*qstr, args, xsink);
}

#ifdef _QORE_HAS_DBI_EXECRAW
static AbstractQoreNode *db2_execRaw(Datasource *ds, const QoreString *qstr, ExceptionSink *xsink) {
   return reinterpret_cast<QoreDB2 *>(ds->getPrivateData())->execRaw(*qstr, xsink);
}
#endif

static AbstractQoreNode *db2_select(Datasource *ds, const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink) {
   return reinterpret_cast<QoreDB2 *>(ds->getPrivateData())->select(*qstr, args, xsink);
}

static AbstractQoreNode *db2_select_rows(Datasource *ds, const QoreString *qstr, const QoreListNode *args, ExceptionSink *xsink) {
   return reinterpret_cast<QoreDB2 *>(ds->getPrivateData())->select_rows(*qstr, args, xsink);
}

static int db2_commit(Datasource *ds, ExceptionSink *xsink) {
   return reinterpret_cast<QoreDB2 *>(ds->getPrivateData())->commit(xsink);
}

static int db2_rollback(Datasource *ds, ExceptionSink *xsink) {
   return reinterpret_cast<QoreDB2 *>(ds->getPrivateData())->rollback(xsink);
}

static int db2_open(Datasource *ds, ExceptionSink *xsink) {
   if (!ds->getDBName()) {
      xsink->raiseException("DATASOURCE-MISSING-DBNAME", "missing dbname in datasource connection");
      return -1;
   }

   std::auto_ptr<QoreDB2> db2(new QoreDB2(*ds, xsink));
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

   // setup code page map
   qore_db2_cp_map[819]  = "iso-8859-1";
   qore_db2_cp_map[912]  = "iso-8859-2";
   qore_db2_cp_map[915]  = "iso-8859-5";
   qore_db2_cp_map[1089] = "iso-8859-6";
   qore_db2_cp_map[813]  = "iso-8859-7";
   qore_db2_cp_map[916]  = "iso-8859-8";
   qore_db2_cp_map[920]  = "iso-8859-9";
   qore_db2_cp_map[923]  = "iso-8859-15";
   qore_db2_cp_map[1051] = "roman8";
   qore_db2_cp_map[1208] = "utf-8";
   qore_db2_cp_map[1167] = "koi8-r";
   qore_db2_cp_map[1168] = "koi8-u";
   qore_db2_cp_map[1250] = "windows-1250";
   qore_db2_cp_map[1251] = "windows-1251";
   qore_db2_cp_map[1252] = "windows-1252";
   qore_db2_cp_map[1253] = "windows-1253";
   qore_db2_cp_map[1254] = "windows-1254";
   qore_db2_cp_map[1255] = "windows-1255";
   qore_db2_cp_map[1256] = "windows-1256";
   qore_db2_cp_map[1257] = "windows-1257";
   qore_db2_cp_map[1258] = "windows-1258";
   qore_db2_cp_map[737]  = "windows-737";
   qore_db2_cp_map[1363] = "windows-1363";
   qore_db2_cp_map[1383] = "gb2312";
   qore_db2_cp_map[1386] = "gbk";
   qore_db2_cp_map[950]  = "big5";
   qore_db2_cp_map[874]  = "tis620";
   qore_db2_cp_map[1392] = "gb18030";
   qore_db2_cp_map[954]  = "eucjp";
   qore_db2_cp_map[5039] = "sjis";
   qore_db2_cp_map[970]  = "euckr";
   qore_db2_cp_map[964]  = "euctw";

   QoreDB2ClientName.sprintf("Qore DB2 driver v%s on Qore v%s %s %s", PACKAGE_VERSION, qore_version_string, qore_target_arch, qore_target_os);

   // DB2 API global initialization
   // turn off thread locking in the DB2 API - all locking will be provided by qore
   SQLRETURN rc = SQLSetEnvAttr(SQL_NULL_HANDLE, SQL_ATTR_PROCESSCTL, (void *)SQL_PROCESSCTL_NOTHREAD, 0);
   if (rc != SQL_SUCCESS) {
      QoreStringNode *err = new QoreStringNode("error initializing DB2 API: SQLSetEnvAttr(SQL_NULL_HANDLE, SQL_ATTR_PROCESSCTL, (void *)SQL_PROCESSCTL_NOTHREAD, 0) returned %d", rc);
      return err;
   }

   // get username
   this_user = getlogin();
   if (!this_user)
      this_user = "<unknown user>";

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
#ifdef _QORE_HAS_DBI_EXECRAW
   methods.add(QDBI_METHOD_EXECRAW, db2_execRaw);
#endif
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
