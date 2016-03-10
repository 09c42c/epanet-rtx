#include "MultiplierTimeSeries.h"

#include <stdlib.h>
#include <boost/foreach.hpp>
#include <boost/range/adaptors.hpp>

using namespace RTX;
using namespace std;



MultiplierTimeSeries::MultiplierTimeSeries() {
  _mode = MultiplierModeMultiply;
}

MultiplierTimeSeries::MultiplierMode MultiplierTimeSeries::multiplierMode() {
  return _mode;
}
void MultiplierTimeSeries::setMultiplierMode(MultiplierTimeSeries::MultiplierMode mode) {
  _mode = mode;
  this->invalidate();
  this->didSetSource(this->source());
}

void MultiplierTimeSeries::didSetSecondary(TimeSeries::_sp secondary) {
  this->didSetSource(this->source());
}

std::set<time_t> MultiplierTimeSeries::timeValuesInRange(RTX::TimeRange range) {
  std::set<time_t> timeSet;
  if (this->clock()) {
    return this->clock()->timeValuesInRange(range);
  }
  if (this->secondary() && this->source()) {
    timeSet = this->source()->timeValuesInRange(range);
    set<time_t> secSet = this->secondary()->timeValuesInRange(range);
    timeSet.insert(secSet.begin(), secSet.end());
  }
  return timeSet;
}

time_t MultiplierTimeSeries::timeBefore(time_t t) {
  if (!this->source() || !this->secondary()) {
    return 0;
  }
  time_t t1 = this->source()->timeBefore(t);
  time_t t2 = this->secondary()->timeBefore(t);
  // compensate for any zero-values (i.e., not-found)
  t1 = (t1 == 0) ? t2 : t1;
  t2 = (t2 == 0) ? t1 : t2;
  return RTX_MIN(t1, t2);
}

time_t MultiplierTimeSeries::timeAfter(time_t t) {
  if (!this->source() || !this->secondary()) {
    return 0;
  }
  time_t t1 = this->source()->timeBefore(t);
  time_t t2 = this->secondary()->timeBefore(t);
  return RTX_MAX(t1, t2);
}


TimeSeries::PointCollection MultiplierTimeSeries::filterPointsInRange(RTX::TimeRange range) {
  
  PointCollection data(vector<Point>(), this->units());
  if (!this->secondary() || !this->source()) {
    return data;
  }
  
  TimeRange queryRange = range;
  if (this->willResample()) {
    // expand range
    queryRange.start = this->source()->timeBefore(range.start + 1);
    queryRange.end = this->source()->timeAfter(range.end - 1);
  }
  queryRange.correctWithRange(range);
  
  PointCollection primary = this->source()->pointCollection(queryRange);
  
  queryRange.start = this->secondary()->timeBefore(queryRange.start);
  queryRange.end = this->secondary()->timeAfter(queryRange.end);
  queryRange.correctWithRange(range);
  
  PointCollection secondary = this->secondary()->pointCollection(queryRange);
  
  set<time_t> pTimes = primary.times();
  set<time_t> sTimes = secondary.times();
  
  set<time_t> combinedTimes;
  combinedTimes.insert(pTimes.begin(), pTimes.end());
  combinedTimes.insert(sTimes.begin(), sTimes.end());

  primary.resample(combinedTimes);
  secondary.resample(combinedTimes);
  
  typedef pair<Point,Point> PointPoint;
  map<time_t, PointPoint> multiplyPoints;
  
  BOOST_FOREACH(const time_t t, combinedTimes) {
    multiplyPoints[t] = make_pair(Point(), Point());
  }
  BOOST_FOREACH(const Point& p, primary.points) {
    multiplyPoints[p.time].first = p;
  }
  BOOST_FOREACH(const Point& p, secondary.points) {
    multiplyPoints[p.time].second = p;
  }
  
  BOOST_FOREACH(const PointPoint& pp, multiplyPoints | boost::adaptors::map_values ) {
    if (pp.first.isValid && pp.second.isValid) {
      Point mp;
      switch (_mode) {
        case MultiplierModeMultiply:
          mp = (pp.first * pp.second).converted(primary.units * secondary.units, this->units());
          break;
        case MultiplierModeDivide:
          mp = (pp.first / pp.second).converted(primary.units / secondary.units, this->units());
          break;
        default:
          break;
      }
      data.points.push_back(mp);
    }
  }
  
  if (this->willResample()) {
    data.resample(this->timeValuesInRange(range));
  }
  
  return data;
}

bool MultiplierTimeSeries::canSetSource(TimeSeries::_sp ts) {
  return true;
}


Units MultiplierTimeSeries::nativeUnits() {
  Units nativeDerivedUnits = RTX_NO_UNITS;
  
  if (!this->source() || !this->secondary()) {
    return RTX_NO_UNITS;
  }
  
  switch (_mode) {
    case MultiplierModeMultiply:
      nativeDerivedUnits = this->source()->units() * this->secondary()->units();
      break;
    case MultiplierModeDivide:
      nativeDerivedUnits = this->source()->units() / this->secondary()->units();
      break;
    default:
      break;
  }
  
  return nativeDerivedUnits;
}

void MultiplierTimeSeries::didSetSource(TimeSeries::_sp ts) {
  if (!this->source() || !this->secondary()) {
    this->setUnits(RTX_DIMENSIONLESS);
    return;
  }
  
  Units nativeDerivedUnits = this->nativeUnits();
  
  if (!this->units().isSameDimensionAs(nativeDerivedUnits) || this->units() == RTX_NO_UNITS) {
    this->setUnits(nativeDerivedUnits);
  }
  
}

bool MultiplierTimeSeries::canChangeToUnits(Units units) {
  if (!this->source() || !this->secondary()) {
    return true; // if the inputs are not fully set, then accept any units.
  }
  else if (units.isSameDimensionAs(this->nativeUnits())) {
    return true;
  }
  else {
    return false;
  }
}


