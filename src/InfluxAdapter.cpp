#include <regex>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/replace.hpp>

#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#include <boost/asio.hpp>
#include <boost/algorithm/string/find.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/join.hpp>

#include <curl/curl.h>

#include "InfluxAdapter.h"
#include "InfluxClient.hpp"
#include "SendPointsCoroutine.hpp"


#include "MetricInfo.h"

#define RTX_INFLUX_CLIENT_TIMEOUT 30

using namespace std;
using namespace RTX;
using namespace oatpp;
using namespace oatpp::web;
using namespace oatpp::network;
using namespace nlohmann;


/***************************************************************************************/
InfluxAdapter::connectionInfo::connectionInfo() {
  proto = "HTTP";
  host = "localhost";
  user = "USER";
  pass = "PASS";
  db = "DB";
  port = 0;
  validate = true;
  msec_ratelimit = 0;
}
/***************************************************************************************/

InfluxAdapter::InfluxAdapter( errCallback_t cb ) : DbAdapter(cb) {
  _inTransaction = false;
  _connected = false;
}
InfluxAdapter::~InfluxAdapter() {
  
}


void InfluxAdapter::setConnectionString(const std::string& str) {
  _RTX_DB_SCOPED_LOCK;
  
  regex kvReg("([^=]+)=([^&\\s]+)&?"); // key - value pair
  // split the tokenized string. we're expecting something like "host=127.0.0.1&port=4242"
  std::map<std::string, std::string> kvPairs;
  {
    auto kv_begin = sregex_iterator(str.begin(), str.end(), kvReg);
    auto kv_end = sregex_iterator();
    for (auto it = kv_begin ; it != kv_end; ++it) {
      kvPairs[(*it)[1]] = (*it)[2];
    }
  }
  
  const map<string, function<void(string)> > 
  kvSetters({
    {"proto", [&](string v){this->conn.proto = v;}},
    {"host", [&](string v){this->conn.host = v;}},
    {"port", [&](string v){this->conn.port = boost::lexical_cast<int>(v);}},
    {"db", [&](string v){this->conn.db = v;}},
    {"u", [&](string v){this->conn.user = v;}},
    {"p", [&](string v){this->conn.pass = v;}},
    {"validate", [&](string v){this->conn.validate = boost::lexical_cast<bool>(v);}},
    {"ratelimit", [&](string v){this->conn.msec_ratelimit = boost::lexical_cast<int>(v);}}
  }); 
  
  for (auto kv : kvPairs) {
    if (kvSetters.count(kv.first) > 0) {
      kvSetters.at(kv.first)(kv.second);
    }
    else {
      cerr << "key not recognized: " << kv.first << " - skipping." << '\n' << flush;
    }
  }
  
  return;
}



void InfluxAdapter::beginTransaction() {
  if (_inTransaction) {
    return;
  }
  _inTransaction = true;
  {
    _RTX_DB_SCOPED_LOCK;
    _transactionLines.clear();
  }
}
void InfluxAdapter::endTransaction() {
  if (!_inTransaction) {
    return;
  }
  this->commitTransactionLines();
  _inTransaction = false;
}

void InfluxAdapter::commitTransactionLines() {
  {
    _RTX_DB_SCOPED_LOCK;
    if (_transactionLines.size() == 0) {
      return;
    }
  }
  {
    string concatLines;
    _RTX_DB_SCOPED_LOCK;
    auto curs = _transactionLines.begin();
    auto end = _transactionLines.end();
    size_t iLine = 0;
    while (curs != end) {
      const string str = *curs;
      ++iLine;
      concatLines.append(str);
      concatLines.append("\n");
      // check for max lines (must respect max lines per send event)
      if (iLine >= this->maxTransactionLines()) {
        // push these lines and clear the queue
        this->sendPointsWithString(concatLines);
        concatLines.clear();
        iLine = 0;
      }
      ++curs;
    }
    if (iLine > 0) {
      // push all/remaining points out
      this->sendPointsWithString(concatLines);
    }
    _transactionLines.clear();
  }
}


bool InfluxAdapter::insertIdentifierAndUnits(const std::string &id, RTX::Units units) {
  
  MetricInfo m(id);
  m.tags.erase("units"); // get rid of units if they are included.
  string properId = m.name();
  
  _idCache.set(properId, units);
  
  // insert a field key/value for something that we won't ever query again.
  // pay attention to bulk operations here, since we may be inserting new ids en-masse
  string tsNameEscaped = influxIdForTsId(id);
  boost::replace_all(tsNameEscaped, " ", "\\ ");
  const string content(tsNameEscaped + " exist=true");
  if (_inTransaction) {
    _RTX_DB_SCOPED_LOCK;
    _transactionLines.push_back(content);
  }
  else {
    this->sendPointsWithString(content);
  }
  // no futher validation.
  return true;
}


void InfluxAdapter::insertSingle(const std::string& id, Point point) {
  this->insertRange(id, {point});
}

void InfluxAdapter::insertRange(const std::string& id, std::vector<Point> points) {
  if (points.size() == 0) {
    return;
  }
  string dbId = influxIdForTsId(id);
  auto content = this->insertionLinesFromPoints(dbId, points);
  
  if (_inTransaction) {
    size_t nLines = 0;
    { // mutex
      _RTX_DB_SCOPED_LOCK;
      for (auto s : content) {
        _transactionLines.push_back(s);
      }
      nLines = _transactionLines.size();
    } // end mutex
    if (nLines > maxTransactionLines()) {
      this->commitTransactionLines();
    }
  }
  else {
    _transactionLines = content;
    this->commitTransactionLines();
  }
  
}


void InfluxAdapter::sendInfluxString(time_t time, const string& seriesId, const string& values) {
  
  string tsNameEscaped = seriesId;
  boost::replace_all(tsNameEscaped, " ", "\\ ");
  
  stringstream ss;
  string timeStr = this->formatTimestamp(time);
  
  ss << tsNameEscaped << " " << values << " " << timeStr;
  string data = ss.str();
  
  
  if (_inTransaction) {
    size_t nLines = 0;
    {
      _RTX_DB_SCOPED_LOCK;
      _transactionLines.push_back(data);
      nLines = _transactionLines.size();
    }
    if (nLines > maxTransactionLines()) {
      this->commitTransactionLines();
    }
  }
  else {
    this->sendPointsWithString(data);
  }
}

string InfluxAdapter::influxIdForTsId(const string& id) {
  // sort named keys into proper order...
  MetricInfo m(id);
  if (m.tags.count("units")) {
    m.tags.erase("units");
  }
  string tsId = m.name();
  if (_idCache.get()->count(tsId) == 0) {
    cerr << "no registered ts with that id: " << tsId << endl;
    // yet i'm being asked for it??
    return "";
  }
  m.tags["units"] = (*_idCache.get())[tsId].second;
  return m.name();
}


vector<string> InfluxAdapter::insertionLinesFromPoints(const string& tsName, vector<Point> points) {
  /*
   As you can see in the example below, you can post multiple points to multiple series at the same time by separating each point with a new line. Batching points in this manner will result in much higher performance.
   
   curl -i -XPOST 'http://localhost:8086/write?db=mydb' --data-binary '
   cpu_load_short,host=server01,region=us-west value=0.64
   cpu_load_short,host=server02,region=us-west value=0.55 1422568543702900257
   cpu_load_short,direction=in,host=server01,region=us-west value=23422.0 1422568543702900257'
   */
  
  // escape any spaces in the tsName
  string tsNameEscaped = tsName;
  boost::replace_all(tsNameEscaped, " ", "\\ ");
  vector<string> outData;
  
  for(const Point& p: points) {
    stringstream ss;
    string valueStr = to_string(p.value); // influxdb 0.10+ supports integers, but only when followed by trailing "i"
    string timeStr = this->formatTimestamp(p.time);
    ss << tsNameEscaped << " value=" << valueStr << "," << "quality=" << (int)p.quality << "i," << "confidence=" << p.confidence << " " << timeStr;
    outData.push_back(ss.str());
  }
  
  return outData;
}

bool InfluxAdapter::assignUnitsToRecord(const std::string& name, const Units& units) {
  return false;
}



/****************************************************************************************************/
/****************************************************************************************************/
/****************************************************************************************************/
const char *kSERIES = "series";
const char *kSHOW_SERIES = "show series";
const char *kERROR = "error";
const char *kRESULTS = "results";
// INFLUX TCP
// task wrapper impl
/**namespace RTX {
  class PplxTaskWrapper : public ITaskWrapper {
  public:
    PplxTaskWrapper();
    pplx::task<void> task;
  };
}
// why the fully private implementation? it's really to guard client applications from having
// to #include the pplx concurrency libs. this way everything is self-contained.
PplxTaskWrapper::PplxTaskWrapper() {
  this->task = pplx::task<void>([]() {
    return; // simple no-op task as filler.
  });
}
#define INFLUX_ASYNC_SEND static_pointer_cast<PplxTaskWrapper>(_sendTask)->task
 **/


std::string InfluxTcpAdapter::Query::selectStr() {
  stringstream ss;
  ss << "SELECT ";
  if (this->select.size() == 0) {
    ss << "*";
  }
  else {
    ss << boost::algorithm::join(this->select,", ");
  }
  ss << " FROM " << this->nameAndWhereClause();
  if (this->order.length() > 0) {
    ss << " ORDER BY " << this->order;
  }
  return ss.str();
}
std::string InfluxTcpAdapter::Query::nameAndWhereClause() {
  stringstream ss;
  ss << this->from;
  if (this->where.size() > 0) {
    ss << " WHERE " << boost::algorithm::join(this->where," AND ");
  }
  string query(curl_escape(ss.str().c_str(), 0));
  return query;
}


map<string, vector<Point> > __pointsFromJson(json& json);
vector<Point> __pointsSingle(json& json);


InfluxTcpAdapter::InfluxTcpAdapter( errCallback_t cb) : InfluxAdapter(cb) {
  //_sendTask.reset(new PplxTaskWrapper());
}

//
InfluxTcpAdapter::InfluxTcpAdapter( errCallback_t cb, std::shared_ptr<InfluxClient> rClient ) : InfluxAdapter(cb){
  this->restClient = rClient;
}

InfluxTcpAdapter::~InfluxTcpAdapter() {
}

shared_ptr<oatpp::web::client::RequestExecutor> InfluxTcpAdapter::createExecutor() {
  if( this->conn.host.compare("localhost") == 0 && this->conn.port == 0){
    auto interface = oatpp::network::virtual_::Interface::obtainShared("virtualhost");
    auto clientConnectionProvider = oatpp::network::virtual_::client::ConnectionProvider::createShared(interface);
    return client::HttpRequestExecutor::createShared(clientConnectionProvider);
  }
  shared_ptr<ClientConnectionProvider> connectionProvider;
  /* Create connection provider */
  if( this->conn.proto.compare("http") == 0 )
  {
    connectionProvider = oatpp::network::tcp::client::ConnectionProvider::createShared({this->conn.host,
      (v_uint16)this->conn.port});
  }
  else if( this->conn.proto.compare("https") == 0)
  {
    auto config = oatpp::openssl::Config::createShared();
    connectionProvider = oatpp::openssl::client::ConnectionProvider::createShared(config, {this->conn.host, (v_uint16)this->conn.port});
  }
  
  /* create connection pool */
  //auto connectionPool = std::make_shared<ClientConnectionPool>(
  //       connectionProvider /* connection provider */,
  //       10 /* max connections */,
  //       std::chrono::seconds(5) /* max lifetime of idle connection */
  //);

  /* create retry policy */
   //auto retryPolicy = std::make_shared<client::SimpleRetryPolicy>(3 /* max retries */, std::chrono::seconds(1) /* retry interval */);

  /* create request executor */
  return client::HttpRequestExecutor::createShared(connectionProvider);
  //return client::HttpRequestExecutor::createShared(connectionPool, retryPolicy /* retry policy */);
}

const DbAdapter::adapterOptions InfluxTcpAdapter::options() const {
  DbAdapter::adapterOptions o;
  
  o.supportsUnitsColumn = true;
  o.supportsSinglyBoundQuery = true;
  o.searchIteratively = false;
  o.canAssignUnits = false;
  o.implementationReadonly = false;
  o.canDoWideQuery = true;
  
  return o;
}

void InfluxTcpAdapter::setConnectionString(const std::string& str) {
  InfluxAdapter::setConnectionString(str);
  /* Create ObjectMapper for serialization of DTOs  */
  auto objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    
  /* Create RequestExecutor which will execute ApiClient's requests */
  //auto requestExecutor = createOatppExecutor();   // <-- Always use oatpp native executor where's possible.
  auto requestExecutor = createExecutor();  // <-- Curl request executor
  
  /* DemoApiClient uses DemoRequestExecutor and json::mapping::ObjectMapper */
  /* ObjectMapper passed here is used for serialization of outgoing DTOs */
  restClient = InfluxClient::createShared(requestExecutor, objectMapper);
}

std::string InfluxTcpAdapter::connectionString() {
  stringstream ss;
  ss << "proto=" << this->conn.proto << "&host=" << this->conn.host << "&port=" << this->conn.port << "&db=" << this->conn.db << "&u=" << this->conn.user << "&p=" << this->conn.pass << "&validate=" << (this->conn.validate ? 1 : 0);
  return ss.str();
}

void InfluxTcpAdapter::doConnect() { 
  _connected = false;
  _errCallback("Connecting...");
  
  // see if the database needs to be created
  bool dbExists = false;
  
  string q("SHOW MEASUREMENTS LIMIT 1");
  auto response = restClient->doQuery(this->conn.getAuthString(), this->conn.db, encodeQuery(q));
  json jsoMeas = jsonFromResponse(response);
  if (!jsoMeas.contains(kRESULTS)) {
    if (jsoMeas.contains("error")) {
      _errCallback(jsoMeas["error"]);
      return;
    }
    else {
      //_errCallback("Connect failed: No Database?");
      return;
    }
  }
  
  json resVal = jsoMeas[kRESULTS];
  if (!resVal.is_array() || resVal.size() == 0) {
    _errCallback("JSON Format Not Recognized");
    return;
  }
  
  // for sure it's an array.
  if (resVal.size() > 0 && resVal[0].contains(kERROR)) {
    _errCallback(resVal[0][kERROR]);
  }
  else {
    dbExists = true;
  }
  
  
  if (!dbExists) {
    string q("CREATE DATABASE " + this->conn.db);
    auto response = restClient->doCreate(encodeQuery(q));
    json js = jsonFromResponse(response);
    if (js.size() == 0 || !js.contains(kRESULTS) ) {
      _errCallback("Can't create database");
      return;
    }
  }
  
  // made it this far? at least we are connected.
  _connected = true;
  _errCallback("OK");
  
  cout << "influx connector: " << this->conn.db << " connected? " << (_connected ? "yes" : "NO") << EOL << flush;
  
  return;

}

IdentifierUnitsList InfluxTcpAdapter::idUnitsList() {
  
  /*
   
   perform a query to get all the series.
   response will be nested in terms of "measurement", and then each array in the "values" array will denote an individual time series:
   
   series: [
   {   name: flow
   columns:  [asset_id, asset_type, dma, ... ]
   values: [ [33410,    pump,       brecon, ...],
   [33453,    pipe,       mt.\ washington, ...],
   [...]
   ]
   },
   {   name: pressure
   columns:   [asset_id, asset_type, dma, ...]
   values: [  [44305,    junction,   brecon, ...],
   [43205,    junction,   mt.\ washington, ...],
   [...]
   ]
   }
   
   */
  
  IdentifierUnitsList ids;
  
  // if i'm busy, then don't bother. unless this could be the first query.
  if (_inTransaction) {
    return _idCache;
  }
  _RTX_DB_SCOPED_LOCK;
  
  
  auto response = restClient->doQuery(this->conn.getAuthString(), this->conn.db, encodeQuery(kSHOW_SERIES));
  json jsv = jsonFromResponse(response);
  
  if (jsv.contains(kRESULTS) &&
      jsv[kRESULTS].is_array() &&
      jsv[kRESULTS].size() > 0 &&
      jsv[kRESULTS][0].contains(kSERIES) &&
      jsv[kRESULTS][0][kSERIES].is_array() ) 
  {
    json seriesArray = jsv["results"][0][kSERIES];
    for (auto seriesIt = seriesArray.begin(); seriesIt != seriesArray.end(); ++seriesIt) {
      
      string str = seriesIt->dump();
      json thisSeries = *seriesIt;
      
      json columns = thisSeries["columns"]; // the only column is "key"
      json values = thisSeries["values"]; // array of single-string arrays
      
      for (auto valuesIt = values.begin(); valuesIt != values.end(); ++valuesIt) {
        json singleStrArr = *valuesIt;
        json dbIdStr = singleStrArr.at(0);
        string dbId = dbIdStr.get<string>();
        boost::replace_all(dbId, "\\ ", " ");
        MetricInfo m(dbId);
        // now we have all kv pairs that define a time series.
        // do we have units info? strip it off before showing the user.
        Units units = RTX_NO_UNITS;
        if (m.tags.count("units") > 0) {
          units = Units::unitOfType(m.tags["units"]);
          // remove units from string name.
          m.tags.erase("units");
        }
        // now assemble the complete name and cache it:
        string properId = m.name();
        ids.set(properId,units);
      } // for each values array (ts definition)
    }
  }
  // else nothing
  _idCache = ids;
  return ids;
}


std::map<std::string, std::vector<Point> > InfluxTcpAdapter::wideQuery(TimeRange range) {
  //_RTX_DB_SCOPED_LOCK;
  
  
  // aggressive prefetch. query all series for some range, then shortcut subsequent queries if they are in the range cached.
  
  // influx allows regex in queries: 
  // select "value" from /.+/ where time > ... and time < ...
  
  vector<string> fields({"time", "value", "quality", "confidence"});
  vector<string> where({"time >= " + to_string(range.start) + "s", "time <= " + to_string(range.end) + "s"});
  
  stringstream ss;
  ss << "SELECT ";
  ss << boost::algorithm::join(fields,", ");
  ss << " FROM /.+/";
  ss << " WHERE " << boost::algorithm::join(where," AND ");
  ss << " GROUP BY * ORDER BY ASC";
  
  // for many wide query optimization needs, we may also want the last known point prior to the range provided
  // such as pump status or other report-by-exception values.
  string prevQuery = "SELECT time, value, quality, confidence FROM /.+/ WHERE time < " + to_string(range.start) + "s GROUP BY * order by time desc limit 1";
  string nextQuery = "SELECT time, value, quality, confidence FROM /.+/ WHERE time > " + to_string(range.end) + "s GROUP BY * order by time asc limit 1";
  
  auto qstr = prevQuery + ";" + ss.str() + ";" + nextQuery;
  auto response = restClient->doQueryWithTimePrecision(this->conn.getAuthString(), this->conn.db, encodeQuery(qstr), "s");
  json jsv = jsonFromResponse(response);
  
  auto fetch = __pointsFromJson(jsv);
  return fetch;
}

// READ
std::vector<Point> InfluxTcpAdapter::selectRange(const std::string& id, TimeRange range) {
  //_RTX_DB_SCOPED_LOCK;
  
  string dbId = influxIdForTsId(id);
  InfluxTcpAdapter::Query q = this->queryPartsFromMetricId(dbId);
  q.where.push_back("time >= " + to_string(range.start) + "s");
  q.where.push_back("time <= " + to_string(range.end) + "s");
  
  auto response = restClient->doQueryWithTimePrecision(this->conn.getAuthString(), this->conn.db, encodeQuery(q.selectStr()), "s");
  json jsv = jsonFromResponse(response);
  return __pointsSingle(jsv);
}

vector<string> _makeSelectStrs(WhereClause q);
vector<string> _makeSelectStrs(WhereClause q) {
  vector<string> clauses;
  for (auto const& i : q.clauses) {
    switch (i.first) {
      case WhereClause::gte:
        clauses.push_back("value >= " + std::to_string(i.second));
        break;
      case WhereClause::gt:
        clauses.push_back("value > " + std::to_string(i.second));
        break;
      case WhereClause::lte:
        clauses.push_back("value <= " + std::to_string(i.second));
        break;
      case WhereClause::lt:
        clauses.push_back("value < " + std::to_string(i.second));
        break;
      default:
        break;
    }
  }
  return clauses;
}

Point InfluxTcpAdapter::selectNext(const std::string& id, time_t time, WhereClause whereClause) {
  //_RTX_DB_SCOPED_LOCK;
  
  std::vector<Point> points;
  string dbId = influxIdForTsId(id);
  Query q = this->queryPartsFromMetricId(dbId);
  q.where.push_back("time > " + to_string(time) + "s");
  q.order = "time asc limit 1";
  
  if (!whereClause.clauses.empty()) {
    auto otherWheres = _makeSelectStrs(whereClause);
    for (auto w : otherWheres) {
      q.where.push_back(w);
    }
  }
  
  auto response = restClient->doQueryWithTimePrecision(this->conn.getAuthString(), this->conn.db, encodeQuery(q.selectStr()), "s");
  json jsv = jsonFromResponse(response);
  points = __pointsSingle(jsv);
  
  if (points.size() == 0) {
    return Point();
  }
  
  return points.front();
}

Point InfluxTcpAdapter::selectPrevious(const std::string& id, time_t time, WhereClause whereClause) {
  //_RTX_DB_SCOPED_LOCK;
  
  std::vector<Point> points;
  string dbId = influxIdForTsId(id);
  
  Query q = this->queryPartsFromMetricId(dbId);
  q.where.push_back("time < " + to_string(time) + "s");
  q.order = "time desc limit 1";
  
  if (!whereClause.clauses.empty()) {
    auto otherWheres = _makeSelectStrs(whereClause);
    for (auto w : otherWheres) {
      q.where.push_back(w);
    }
  }
  
  auto response = restClient->doQueryWithTimePrecision(this->conn.getAuthString(), this->conn.db, encodeQuery(q.selectStr()), "s");
  json jsv = jsonFromResponse(response);
  points = __pointsSingle(jsv);
  
  if (points.size() == 0) {
    return Point();
  }
  
  return points.front();
}



vector<Point> InfluxTcpAdapter::selectWithQuery(const std::string& query, TimeRange range) {
  //_RTX_DB_SCOPED_LOCK;
  // expects a "$timeFilter" placeholder, to be replaced with the time range, e.g., "time >= t1 and time <= t2"
  
  //case insensitive find
  if (boost::ifind_first(query, std::string("$timeFilter")).empty()) {
    // add WHERE clause
    return vector<Point>();
  }
  
  string qStr = query;
  
  stringstream tfss;
  if (range.start > 0) {
    tfss << "time >= " << range.start << "s";
  }
  if (range.start > 0 && range.end > 0) {
    tfss << " and ";
  }
  if (range.end > 0) {
    tfss << "time <= " << range.end << "s";
  }
  string timeFilter = tfss.str();
  
  boost::replace_all(qStr, "$timeFilter", timeFilter);
  
  if (range.start == 0) {
    qStr += " order by desc limit 1";
  }
  else if (range.end == 0) {
    qStr += " order by asc limit 1";
  }
  else {
    qStr += " order by asc";
  }

  auto response = restClient->doQueryWithTimePrecision(this->conn.getAuthString(), this->conn.db, encodeQuery(qStr), "s");
  json jsv = jsonFromResponse(response);
  auto points = __pointsSingle(jsv);
  return points;
}

// DELETE
void InfluxTcpAdapter::removeRecord(const std::string& id) {
  const string dbId = this->influxIdForTsId(id);
  Query q = this->queryPartsFromMetricId(id);
  
  stringstream sqlss;
  sqlss << "DROP SERIES FROM " << q.nameAndWhereClause();
  oatpp::String qStr(sqlss.str());
  restClient->removeRecord(this->conn.getAuthString(), encodeQuery(qStr));
}

void InfluxTcpAdapter::removeAllRecords() {
  
  _errCallback("Truncating");
  OATPP_LOGD(TAG, "Truncating");
  
  auto ids = this->idUnitsList();
  
  stringstream sqlss;
  sqlss << "DROP DATABASE " << this->conn.db << "; CREATE DATABASE " << this->conn.db;
  string qStr(sqlss.str());
  auto response = restClient->removeRecord(this->conn.getAuthString(), encodeQuery(qStr));
  json v = jsonFromResponse(response);
  
  this->beginTransaction();
  for (auto ts_units : *ids.get()) {
    this->insertIdentifierAndUnits(ts_units.first, ts_units.second.first);
  }
  this->endTransaction();
  
  _errCallback("OK");
  return;
}

size_t InfluxTcpAdapter::maxTransactionLines() {
  return 5000;
}

void InfluxTcpAdapter::sendPointsWithString(const std::string& content) {
  //INFLUX_ASYNC_SEND.wait(); // wait on previous send if needed.
  
  const string bodyContent(content);

  namespace bio = boost::iostreams;
  std::stringstream compressed;
  std::stringstream origin(bodyContent);
  bio::filtering_streambuf<bio::input> out;
  out.push(bio::gzip_compressor(bio::gzip_params(bio::gzip::default_compression)));
  out.push(origin);
  bio::copy(out, compressed);
  const string zippedContent(compressed.str());
  
  oatpp::async::Executor executor;

  executor.execute<SendPointsCoroutine>(restClient, this->conn.getAuthString(), "gzip", this->conn.db, "s", zippedContent);

  executor.waitTasksFinished();
  executor.stop();
  executor.join();

}

string InfluxTcpAdapter::encodeQuery(string queryString){
  string query(curl_escape(queryString.c_str(), 0));
  return query;
}

json InfluxTcpAdapter::jsonFromResponse(const std::shared_ptr<Response> response) {
  json js = json::object();
  
  auto errCallback = _errCallback;
  auto connection = this->conn;
  
  int code = response->getStatusCode();
  if(code == 200){
    OATPP_LOGI(TAG, "Connected");
    std::string bodyStr = response->readBodyToString().getValue("");
    OATPP_LOGD(TAG, "%s", bodyStr.c_str());
    js = json::parse(bodyStr);
    return js;
  }else{
    OATPP_LOGE(TAG, "Connection Error: %s", response->getStatusDescription()->c_str());
    return js;
  }
}


vector<Point> __pointsSingle(json& json) {
  auto multi = __pointsFromJson(json);
  if (multi.size() > 0) {
    return multi.begin()->second;
  }
  else {
    return vector<Point>();
  }
}


map<string, vector<Point> > __pointsFromJson(json& json) {
  
  map<string, vector<Point> > out;
  
  // check for correct response format:
  if (!json.is_object() || 
      json.size() == 0 ||
      !json.contains(kRESULTS) ||
      !json[kRESULTS].is_array() ||
      json[kRESULTS].size() == 0)
  {
    return out;
  }
  
  
  for (auto &statement : json[kRESULTS]) {
    
    if ( !statement.is_object() || !statement.contains(kSERIES) ) {
      continue;
    }
    auto &seriesArray = statement[kSERIES];
    for (const auto &series : seriesArray) {
      // assemble the proper identifier for this series
      MetricInfo metric("");
      metric.measurement = series.at("name");
      if (series.contains("tags")) {
        auto tagsObj = series.at("tags");
        json::iterator tagsIter = tagsObj.begin();
        while (tagsIter != tagsObj.end()) {
          metric.tags[tagsIter.key()] = tagsIter.value();
          ++tagsIter;
        }
        Units units = Units::unitOfType(metric.tags.at("units"));
        metric.tags.erase("units"); // get rid of units if they are included.
      }
      string properId = metric.name();
      
      map<string,int> columnMap;
      auto cols = series.at("columns");
      for (int i = 0; i < cols.size(); ++i) {
        string colName = cols[i];
        columnMap[colName] = (int)i;
      }
      
      // check columns are all there
      bool allColumnsPresent = true;
      for (const string &key : {"time","value","quality","confidence"}) {
        if (columnMap.count(key) == 0) {
          cerr << "column map does not contain key: " << key << endl;
          allColumnsPresent = false;
        }
      }
      if (!allColumnsPresent) {
        continue; // skip this parsing iteration
      }
      
      const int
      timeIndex = columnMap["time"],
      valueIndex = columnMap["value"],
      qualityIndex = columnMap["quality"],
      confidenceIndex = columnMap["confidence"];
      
      const auto &values = series.at("values");
      
      auto nValues = values.size();
      if (nValues == 0) {
        continue;
      }
      
      if (out.count(properId) == 0) {
        out[properId] = vector<Point>();
      }
      
      auto pointVec = &(out.at(properId));
      
      if (nValues > 1) {
        pointVec->reserve(pointVec->size() + nValues + 2);
      }
      
      
      for (const auto &rowV : values) {
        const auto &row = rowV;
        time_t t = row.at(timeIndex);
        double v = row.at(valueIndex);
        Point::PointQuality q = Point::opc_rtx_override;
        if (!row.at(qualityIndex).is_null()) {
          q = (Point::PointQuality)(row.at(qualityIndex));
        }
        double c = 0;
        if (!row.at(confidenceIndex).is_null()) {
          c = row.at(confidenceIndex);
        }
        pointVec->push_back(Point(t,v,q,c));
      }
    }
  }
  
  // sort these points
  for (auto &ts : out) {
    std::sort(ts.second.begin(), ts.second.end(), Point::comparePointTime);
  }
  
  
  return out;
}




InfluxTcpAdapter::Query InfluxTcpAdapter::queryPartsFromMetricId(const std::string &name) {
  MetricInfo m(name);
  
  Query q;
  q.select = {"time", "value", "quality", "confidence"};
  q.from = "\"" + m.measurement + "\"";
  
  if (m.tags.size() > 0) {
    for( auto p : m.tags) {
      stringstream ss;
      ss << "\"" << p.first << "\"='" << p.second << "'";
      string s = ss.str();
      q.where.push_back(s);
    }
  }
  
  return q;
}

std::string InfluxTcpAdapter::formatTimestamp(time_t t) {
  return to_string(t);
}


/****************************************************************************************************/
/****************************************************************************************************/
/****************************************************************************************************/
/****************************************************************************************************/


InfluxUdpAdapter::InfluxUdpAdapter( errCallback_t cb ) : InfluxAdapter(cb) {
  _sendFuture = std::async(launch::async, [&](){return;});
}

InfluxUdpAdapter::~InfluxUdpAdapter() {
  
}

const DbAdapter::adapterOptions InfluxUdpAdapter::options() const {
  DbAdapter::adapterOptions o;
  
  o.supportsUnitsColumn = true;
  o.supportsSinglyBoundQuery = true;
  o.searchIteratively = false;
  o.canAssignUnits = false;
  o.implementationReadonly = false;
  o.canDoWideQuery = false;
  
  return o;
}

std::string InfluxUdpAdapter::connectionString() {
  stringstream ss;
  ss << "host=" << this->conn.host << "&port=" << this->conn.port << "&ratelimit=" << this->conn.msec_ratelimit;
  return ss.str();
}

void InfluxUdpAdapter::doConnect() {
  _connected = false;
  boost::asio::io_service io_service;
  try {
    using boost::asio::ip::udp;
    boost::asio::io_service io_service;
    udp::resolver resolver(io_service);
    udp::resolver::query query(udp::v4(), this->conn.host, to_string(this->conn.port));
    udp::endpoint receiver_endpoint = *resolver.resolve(query);
    udp::socket socket(io_service);
    socket.open(udp::v4());
    socket.close();
  } catch (const std::exception &err) {
    DebugLog << "could not connect to UDP endpoint" << EOL << flush;
    _errCallback("Invalid UDP Endpoint");
    return;
  }
  _errCallback("Connected");
  _connected = true;
}

IdentifierUnitsList InfluxUdpAdapter::idUnitsList() {
  return IdentifierUnitsList();
}

// READ
std::vector<Point> InfluxUdpAdapter::selectRange(const std::string& id, TimeRange range) {
  return {Point()};
}

Point InfluxUdpAdapter::selectNext(const std::string& id, time_t time, WhereClause q) {
  return Point();
}

Point InfluxUdpAdapter::selectPrevious(const std::string& id, time_t time, WhereClause q) {
  return Point();
}


// DELETE
void InfluxUdpAdapter::removeRecord(const std::string& id) {
  return;
}

void InfluxUdpAdapter::removeAllRecords() {
  return;
}

size_t InfluxUdpAdapter::maxTransactionLines() {
  return 10;
}

void InfluxUdpAdapter::sendPointsWithString(const std::string& content) {
  if (_sendFuture.valid()) {
    _sendFuture.wait();
  }
  string body(content);
  _sendFuture = std::async(launch::async, [&,body]() {
    using boost::asio::ip::udp;
    boost::asio::io_service io_service;
    udp::resolver resolver(io_service);
    udp::resolver::query query(udp::v4(), this->conn.host, to_string(this->conn.port));
    udp::endpoint receiver_endpoint = *resolver.resolve(query);
    udp::socket socket(io_service);
    socket.open(udp::v4());
    boost::system::error_code err;
    socket.send_to(boost::asio::buffer(body, body.size()), receiver_endpoint, 0, err);
    if (err) {
      DebugLog << "UDP SEND ERROR: " << err.message() << EOL << flush;
    }
    socket.close();
    if (conn.msec_ratelimit > 0) {
      this_thread::sleep_for(chrono::milliseconds(conn.msec_ratelimit));
    }
  });

}

std::string InfluxUdpAdapter::formatTimestamp(time_t t) {
  // Line protocol requires unix-nano unles qualified by HTTP-GET fields,
  // which of course we don't have over UDP
  return to_string(t) + "000000000";
}
