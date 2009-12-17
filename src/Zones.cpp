/*
 * Copyright (c) 2006 Sean C. Rhea (srhea@srhea.net)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <QMessageBox>
#include "Zones.h"
#include "TimeUtils.h"
#include <QtGui>
#include <QtAlgorithms>
#include <qcolor.h>
#include <assert.h>
#include <math.h>

static QList <int> zone_default;
static QList <bool> zone_default_is_pct;
static QList <QString> zone_default_name;
static QList <QString> zone_default_desc;
static int nzones_default = 0;

// the infinity endpoints are indicated with empty dates
static const QDate date_zero     = QDate::QDate();
static const QDate date_infinity = QDate::QDate();

// functions used for sorting zones and ranges
bool Zones::zone_default_index_lessthan(int i1, int i2) {
    return (zone_default[i1] * (zone_default_is_pct[i1] ? 250 : 1) <
	    zone_default[i2] * (zone_default_is_pct[i2] ? 250 : 1));
}

bool Zones::zoneptr_lessthan(const ZoneInfo &z1, const ZoneInfo &z2) {
    return ((z1.lo < z2.lo) ||
	    ((z1.lo == z2.lo) && (z1.hi < z2.hi)));
}

bool Zones::rangeptr_lessthan(const ZoneRange &r1, const ZoneRange &r2) {
    return (((! r2.begin.isNull()) &&
	     ( r1.begin.isNull() || r1.begin < r2.begin )) ||
	    ((r1.begin == r2.begin) &&
	     (! r1.end.isNull()) &&
	     ( r2.end.isNull() || r1.end < r2.end )));
}

// initialize default static zone parameters
void Zones::initializeZoneParameters()
{
    static int initial_zone_default[] = {
        0, 55, 75, 90, 105, 120, 150
    };
    static const char *initial_zone_default_desc[] = {
        "Active Recovery", "Endurance", "Tempo", "Threshold",
        "VO2Max", "Anaerobic", "Neuromuscular"
    };
    static const char *initial_zone_default_name[] = {
        "Z1", "Z2", "Z3", "Z4", "Z5", "Z6", "Z7"
    };

    static int initial_nzones_default =
	sizeof(initial_zone_default) /
	sizeof(initial_zone_default[0]);

    zone_default.clear();
    zone_default_is_pct.clear();
    zone_default_desc.clear();
    zone_default_name.clear();
    nzones_default = 0;

    nzones_default = initial_nzones_default;

    for (int z = 0; z < nzones_default; z ++) {
	zone_default.append(initial_zone_default[z]);
	zone_default_is_pct.append(true);
	zone_default_name.append(QString(initial_zone_default_name[z]));
	zone_default_desc.append(QString(initial_zone_default_desc[z]));
    }
}

// read zone file, allowing for zones with or without end dates
bool Zones::read(QFile &file)
{
    defaults_from_user = false;
    zone_default.clear();
    zone_default_is_pct.clear();
    zone_default_name.clear();
    zone_default_desc.clear();
    nzones_default = 0;

    // set up possible warning dialog
    warning = QString();
    int warning_lines = 0;
    const int max_warning_lines = 100;

    // macro to append lines to the warning
    #define append_to_warning(s) \
        if (warning_lines < max_warning_lines) \
	    warning.append(s);  \
        else if (warning_lines == max_warning_lines) \
	    warning.append("...\n"); \
        warning_lines ++;

    // read using text mode takes care of end-lines
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        err = "can't open file";
        return false;
    }
    QTextStream fileStream(&file);

    QRegExp commentrx("\\s*#.*$");
    QRegExp blankrx("^[ \t]*$");
    QRegExp rangerx[] = {
        QRegExp("^\\s*(?:from\\s+)?"                                 // optional "from"
                "((\\d\\d\\d\\d)[-/](\\d{1,2})[-/](\\d{1,2})|BEGIN)" // begin date
                "\\s*([,:]?\\s*(FTP|CP)\\s*=\\s*(\\d+))?"            // optional {CP/FTP = integer (optional %)}
                "\\s*:?\\s*$",                                       // optional :
                Qt::CaseInsensitive),
        QRegExp("^\\s*(?:from\\s+)?"                                 // optional "from"
                "((\\d\\d\\d\\d)[-/](\\d{1,2})[-/](\\d{1,2})|BEGIN)" // begin date
                "\\s+(?:until|to|-)\\s+"                             // until
                "((\\d\\d\\d\\d)[-/](\\d{1,2})[-/](\\d{1,2})|END)?"  // end date
                "\\s*:?,?\\s*((FTP|CP)\\s*=\\s*(\\d+))?"          // optional {CP/FTP = integer (optional %)}
                "\\s*:?\\s*$",                                       // optional :
                Qt::CaseInsensitive)
    };
    QRegExp zonerx("^\\s*([^ ,][^,]*),\\s*([^ ,][^,]*),\\s*"
		   "(\\d+)\\s*(%?)\\s*(?:,\\s*(\\d+|MAX)\\s*(%?)\\s*)?$",
		   Qt::CaseInsensitive);
    QRegExp zonedefaultsx("^\\s*(?:zone)?\\s*defaults?\\s*:?\\s*$",
			  Qt::CaseInsensitive);

    int lineno = 0;

    // the current range in the file
    // ZoneRange *range = NULL;
    bool in_range = false;
    QDate begin, end;
    int cp;
    QList<ZoneInfo> zoneInfos;

    // true if zone defaults are found in the file (then we need to write them)
    bool zones_are_defaults = false;

    while (! fileStream.atEnd() ) {
        ++lineno;
	QString line = fileStream.readLine();
        int pos = commentrx.indexIn(line, 0);
        if (pos != -1)
            line = line.left(pos);
        if (blankrx.indexIn(line, 0) == 0)
	    goto next_line;

	// check for default zone range definition (may be followed by zone definitions)
	if (zonedefaultsx.indexIn(line, 0) != -1) {
	    zones_are_defaults = true;

	    // defaults are allowed only at the beginning of the file
	    if (ranges.size()) {
		err = "Zone defaults must be specified at head of power.zones file";
		return false;
	    }

	    // only one set of defaults is allowed
	    if (nzones_default) {
		err = "Only one set of zone defaults may be specified in power.zones file";
		return false;
	    }

	    goto next_line;
	}

	// check for range specification (may be followed by zone definitions)
	for (int r = 0; r < 2; r++) {
	    if (rangerx[r].indexIn(line, 0) != -1) {

		if (in_range) {
		    // if zones are empty, then generate them
                    ZoneRange range(begin, end, cp);
                    range.zones = zoneInfos;
		    if (range.zones.empty()) {
			if (range.cp > 0)
			    setZonesFromCP(range);
			else {
			    err = tr("line %1: read new range without reading "
				     "any zones for previous one").arg(lineno);
			    file.close();
			    return false;
			}
		    }
		    // else sort them
		    else {
			qSort(range.zones.begin(), range.zones.end(), zoneptr_lessthan);
		    }

		    ranges.append(range);
		}

                in_range = true;
		zones_are_defaults = false;
                zoneInfos.clear();

		// QDate begin, end;

		// process the beginning date
		if (rangerx[r].cap(1) == "BEGIN")
		    begin = date_zero;
		else {
		    begin = QDate(rangerx[r].cap(2).toInt(),
				  rangerx[r].cap(3).toInt(),
				  rangerx[r].cap(4).toInt()
				  );
		}

		// process an end date, if any, else it is null
		if (rangerx[r].cap(5) == "END")
		    end = date_infinity;
		else if (rangerx[r].cap(6).toInt() || rangerx[r].cap(7).toInt() || rangerx[r].cap(8).toInt()) {
		    end = QDate(rangerx[r].cap(6).toInt(),
				rangerx[r].cap(7).toInt(),
				rangerx[r].cap(8).toInt()
				);
		}
		else {
		    end = QDate();
		}

		// set up the range, capturing CP if it's specified
		// range = new ZoneRange(begin, end);
		int nCP = (r ? 11 : 7);
		if (rangerx[r].numCaptures() == (nCP))
		    cp = rangerx[r].cap(nCP).toInt();
                else
                    cp = 0;
		goto next_line;
	    }
	}

	// check for zone definition
        if (zonerx.indexIn(line, 0) != -1) {
	    if (! (in_range || zones_are_defaults)) {
		err = tr("line %1: read zone without "
			 "preceeding date range").arg(lineno);
		file.close();
		return false;
	    }

	    int lo = zonerx.cap(3).toInt();

	    // allow for zone specified as % of CP
	    bool lo_is_pct = false;
	    if (zonerx.cap(4) == "%")
		if (zones_are_defaults)
		    lo_is_pct = true;
		else if (cp > 0)
		    lo = int(lo * cp / 100);
		else {
		    err = tr("attempt to set zone based on % of "
			     "CP without setting CP in line number %1.\n").
			arg(lineno);
		    file.close();
		    return false;
		}

	    int hi;
	    // if this is not a zone defaults specification, process possible hi end of zones
	    if (zones_are_defaults || zonerx.cap(5).isEmpty())
		hi = -1;   // signal an undefined number
	    else if (zonerx.cap(5) == "MAX")
		hi = INT_MAX;
	    else {
		hi = zonerx.cap(5).toInt();

		// allow for zone specified as % of CP
		if (zonerx.cap(5) == "%")
		    if (cp > 0)
			hi = int(hi * cp / 100);
		    else {
			err = tr("attempt to set zone based on % of CP "
				 "without setting CP in line number %1.\n").
			    arg(lineno);
			file.close();
			return false;
		    }
	    }

	    if (zones_are_defaults) {
		nzones_default ++;
		zone_default_is_pct.append(lo_is_pct);
		zone_default.append(lo);
		zone_default_name.append(zonerx.cap(1));
		zone_default_desc.append(zonerx.cap(2));
		defaults_from_user = true;
	    }
	    else {
                if (hi <= lo) {
                    err = tr("High value must be greater than low value for "
                             "zone definition on line %1 of power.zones").arg(lineno);
                    file.close();
                    return false;
                }
		ZoneInfo zone(zonerx.cap(1), zonerx.cap(2), lo, hi);
		zoneInfos.append(zone);
	    }
	}
    next_line: {}
    }

    if (in_range) {
        ZoneRange range(begin, end, cp);
        range.zones = zoneInfos;
        if (range.zones.empty()) {
            if (range.cp > 0)
	        setZonesFromCP(range);
	    else {
                err = tr("file ended without reading any zones for last range");
                file.close();
                return false;
	    }
        }
	else {
	    qSort(range.zones.begin(), range.zones.end(), zoneptr_lessthan);
	}

        ranges.append(range);
    }
    file.close();

    // sort the ranges
    qSort(ranges.begin(), ranges.end(), rangeptr_lessthan);

    // sort the zone defaults, as best we can (may not be right if there's
    // a mix of % and absolute ranges)
    if (nzones_default)  {
	QVector <int> zone_default_index(nzones_default);
	for (int i = 0; i < nzones_default; i++)
	    zone_default_index[i] = i;
	qSort(zone_default_index.begin(), zone_default_index.end(), zone_default_index_lessthan);

	QVector <int> zone_default_new(nzones_default);
	QVector <bool> zone_default_is_pct_new(nzones_default);
	QVector <QString> zone_default_name_new(nzones_default);
	QVector <QString> zone_default_desc_new(nzones_default);

	for (int i = 0; i < nzones_default; i++) {
	    zone_default_new[i]        = zone_default[zone_default_index[i]];
	    zone_default_is_pct_new[i] = zone_default_is_pct[zone_default_index[i]];
	    zone_default_name_new[i]   = zone_default_name[zone_default_index[i]];
	    zone_default_desc_new[i]   = zone_default_desc[zone_default_index[i]];
	}

	for (int i = 0; i < nzones_default; i++) {
	    zone_default[i]            = zone_default_new[i];
	    zone_default_is_pct[i]     = zone_default_is_pct_new[i];
	    zone_default_name[i]       = zone_default_name_new[i];
	    zone_default_desc[i]       = zone_default_desc_new[i];
	}
    }

    // resolve undefined endpoints in ranges and zones
    for (int nr = 0; nr < ranges.size(); nr ++) {
	// clean up gaps or overlaps in zone ranges
	if (ranges[nr].end.isNull())
	    ranges[nr].end =
		(nr < ranges.size() - 1) ?
		ranges[nr + 1].begin :
		date_infinity;
	else if ((nr < ranges.size() - 1) &&
		 (ranges[nr + 1].begin != ranges[nr].end)) {

	    append_to_warning(tr("Setting end date of range %1 "
				 "to start date of range %2.\n").
			      arg(nr + 1).
			      arg(nr + 2)
			      );

	    ranges[nr].end = ranges[nr + 1].begin;
	}
	else if ((nr == ranges.size() - 1) &&
		 (ranges[nr].end < QDate::currentDate())) {

	    append_to_warning(tr("Extending final range %1 to infinite "
				 "to include present date.\n").arg(nr + 1));

	    ranges[nr].end = date_infinity;
	}

	if (ranges[nr].zones.size()) {
	    // check that the first zone starts with zero
	    ranges[nr].zones[0].lo = 0;

	    // resolve zone end powers
	    for (int nz = 0; nz < ranges[nr].zones.size(); nz ++) {
		if (ranges[nr].zones[nz].hi == -1)
		    ranges[nr].zones[nz].hi =
			(nz < ranges[nr].zones.size() - 1) ?
			ranges[nr].zones[nz + 1].lo :
			INT_MAX;
		else if ((nz < ranges[nr].zones.size() - 1) &&
			 (ranges[nr].zones[nz].hi != ranges[nr].zones[nz + 1].lo)) {
		    if (abs(ranges[nr].zones[nz].hi - ranges[nr].zones[nz + 1].lo) > 4)
			append_to_warning(tr("Range %1: matching top of zone %2 "
					     "(%3) to bottom of zone %4 (%5).\n").
					  arg(nr + 1).
					  arg(ranges[nr].zones[nz].name).
					  arg(ranges[nr].zones[nz].hi).
					  arg(ranges[nr].zones[nz + 1].name).
					  arg(ranges[nr].zones[nz + 1].lo)
					  );
		    ranges[nr].zones[nz].hi = ranges[nr].zones[nz + 1].lo;
		}
		else if ((nz == ranges[nr].zones.size() - 1) &&
			 (ranges[nr].zones[nz].hi < INT_MAX)) {
		    append_to_warning(tr("Range %1: setting top of zone %2 from %3 to MAX.\n").
				      arg(nr + 1).
				      arg(ranges[nr].zones[nz].name).
				      arg(ranges[nr].zones[nz].hi)
				      );
		    ranges[nr].zones[nz].hi = INT_MAX;
		}
            }
	}
    }

    // mark zones as modified so pages which depend on zones can be updated
    modificationTime = QDateTime::currentDateTime();

    return true;
}

// note empty dates are treated as automatic matches for begin or
// end of range
int Zones::whichRange(const QDate &date) const
{
    for (int rnum = 0; rnum < ranges.size(); ++rnum) {
        const ZoneRange &range = ranges[rnum];
        if (((date >= range.begin) || (range.begin.isNull())) &&
            ((date < range.end) || (range.end.isNull())))
            return rnum;
    }
    return -1;
}

int Zones::numZones(int rnum) const
{
    assert(rnum < ranges.size());
    return ranges[rnum].zones.size();
}

int Zones::whichZone(int rnum, double value) const
{
    assert(rnum < ranges.size());
    const ZoneRange &range = ranges[rnum];
    for (int j = 0; j < range.zones.size(); ++j) {
        const ZoneInfo &info = range.zones[j];
	// note: the "end" of range is actually in the next zone
        if ((value >= info.lo) && (value < info.hi))
            return j;
    }
    return -1;
}

void Zones::zoneInfo(int rnum, int znum,
                     QString &name, QString &description,
                     int &low, int &high) const
{
    assert(rnum < ranges.size());
    const ZoneRange &range = ranges[rnum];
    assert(znum < range.zones.size());
    const ZoneInfo &zone = range.zones[znum];
    name = zone.name;
    description = zone.desc;
    low = zone.lo;
    high = zone.hi;
}

int Zones::getCP(int rnum) const
{
    assert(rnum < ranges.size());
    return ranges[rnum].cp;
}

void Zones::setCP(int rnum, int cp)
{
    ranges[rnum].cp = cp;
    modificationTime = QDateTime::currentDateTime();
}

// generate a list of zones from CP
int Zones::lowsFromCP(QList <int> *lows, int cp) const {
    if (nzones_default == 0)
	initializeZoneParameters();

    lows->clear();

    for (int z = 0; z < nzones_default; z++)
	lows->append(zone_default_is_pct[z] ? zone_default[z] * cp / 100 : zone_default[z]);

    return nzones_default;
}

// access the zone name
QString Zones::getDefaultZoneName(int z) const {
    return zone_default_name[z];
}

// access the zone description
QString Zones::getDefaultZoneDesc(int z) const {
    return zone_default_desc[z];
}

// set the zones from the CP value (the cp variable)
void Zones::setZonesFromCP(ZoneRange &range) {
    range.zones.clear();

    if (nzones_default == 0)
	initializeZoneParameters();

    for (int i = 0; i < nzones_default; i++) {
	int lo = zone_default_is_pct[i] ? zone_default[i] * range.cp / 100 : zone_default[i];
	int hi = lo;
	ZoneInfo zone(zone_default_name[i], zone_default_desc[i], lo, hi);
	range.zones.append(zone);
    }

    // sort the zones (some may be pct, others absolute, so zones need to be sorted,
    // rather than the defaults
    qSort(range.zones.begin(), range.zones.end(), zoneptr_lessthan);

    // set zone end dates
    for (int i = 0; i < range.zones.size(); i ++)
	range.zones[i].hi =
	    (i < nzones_default - 1) ?
	    range.zones[i + 1].lo :
	    INT_MAX;

    // mark that the zones were set from CP, so if zones are subsequently
    // written, only CP is saved
    range.zonesSetFromCP = true;
}

void Zones::setZonesFromCP(int rnum) {
    assert((rnum >= 0) && (rnum < ranges.size()));
    setZonesFromCP(ranges[rnum]);
}

// return the list of starting values of zones for a given range
QList <int> Zones::getZoneLows(int rnum) const {
    if (rnum >= ranges.size())
        return QList <int>::QList();
    const ZoneRange &range = ranges[rnum];
    QList <int> return_values;
    for (int i = 0; i < range.zones.size(); i ++)
        return_values.append(ranges[rnum].zones[i].lo);
    return return_values;
}

// return the list of ending values of zones for a given range
QList <int> Zones::getZoneHighs(int rnum) const {
    if (rnum >= ranges.size())
        return QList <int>::QList();
    const ZoneRange &range = ranges[rnum];
    QList <int> return_values;
    for (int i = 0; i < range.zones.size(); i ++)
        return_values.append(ranges[rnum].zones[i].hi);
    return return_values;
}

// return the list of zone names
QList <QString> Zones::getZoneNames(int rnum) const {
    if (rnum >= ranges.size())
        return QList <QString>::QList();
    const ZoneRange &range = ranges[rnum];
    QList <QString> return_values;
    for (int i = 0; i < range.zones.size(); i ++)
        return_values.append(ranges[rnum].zones[i].name);
    return return_values;
}

QString Zones::summarize(int rnum, QVector<double> &time_in_zone) const
{
    assert(rnum < ranges.size());
    const ZoneRange &range = ranges[rnum];
    assert(time_in_zone.size() == range.zones.size());
    QString summary;
    if(range.cp > 0){
        summary += "<table align=\"center\" width=\"70%\" border=\"0\">";
        summary += "<tr><td align=\"center\">";
        summary += tr("Critical Power: %1").arg(range.cp);
        summary += "</td></tr></table>";
    }
    summary += "<table align=\"center\" width=\"70%\" ";
    summary += "border=\"0\">";
    summary += "<tr>";
    summary += "<td align=\"center\">Zone</td>";
    summary += "<td align=\"center\">Description</td>";
    summary += "<td align=\"center\">Low</td>";
    summary += "<td align=\"center\">High</td>";
    summary += "<td align=\"center\">Time</td>";
    summary += "</tr>";
    QColor color = QApplication::palette().alternateBase().color();
    color = QColor::fromHsv(color.hue(), color.saturation() * 2, color.value());
    for (int zone = 0; zone < time_in_zone.size(); ++zone) {
        if (time_in_zone[zone] > 0.0) {
            QString name, desc;
            int lo, hi;
            zoneInfo(rnum, zone, name, desc, lo, hi);
            if (zone % 2 == 0)
                summary += "<tr bgcolor='" + color.name() + "'>";
            else
                summary += "<tr>";
            summary += QString("<td align=\"center\">%1</td>").arg(name);
            summary += QString("<td align=\"center\">%1</td>").arg(desc);
            summary += QString("<td align=\"center\">%1</td>").arg(lo);
            if (hi == INT_MAX)
                summary += "<td align=\"center\">MAX</td>";
            else
                summary += QString("<td align=\"center\">%1</td>").arg(hi);
            summary += QString("<td align=\"center\">%1</td>")
                .arg(time_to_string((unsigned) round(time_in_zone[zone])));
            summary += "</tr>";
        }
    }
    summary += "</table>";
    return summary;
}

#define USE_SHORT_POWER_ZONES_FORMAT true   /* whether a less redundent format should be used */
void Zones::write(QDir home)
{
    QString strzones;

    // write the defaults if they were specified by the user
    if (defaults_from_user) {
	strzones += QString("DEFAULTS:\n");
	for (int z = 0 ; z < nzones_default; z ++)
	    strzones += QString("%1,%2,%3%4\n").
		arg(zone_default_name[z]).
		arg(zone_default_desc[z]).
		arg(zone_default[z]).
		arg(zone_default_is_pct[z]?"%":"");
	strzones += QString("\n");
    }
    for (int i = 0; i < ranges.size(); i++) {
        int cp = getCP(i);

	// print header for range
	// note this explicitly sets the first and last ranges such that all time is spanned

        #if USE_SHORT_POWER_ZONES_FORMAT
        if (i == 0)
            strzones += QString("BEGIN: CP=%1").arg(cp);
	else
            strzones += QString("%1: CP=%2").arg(getStartDate(i).toString("yyyy/MM/dd")).arg(cp);
	strzones += QString("\n");

	// step through and print the zones if they've been explicitly set
	if (! ranges[i].zonesSetFromCP) {
	    for (int j = 0; j < ranges[i].zones.size(); j ++) {
                const ZoneInfo &zi = ranges[i].zones[j];
		strzones += QString("%1,%2,%3\n").arg(zi.name).arg(zi.desc).arg(zi.lo);
            }
	    strzones += QString("\n");
	}
        #else
        if(ranges.size() <= 1)
            strzones += QString("FROM BEGIN UNTIL END, CP=%1:").arg(cp);
        else if (i == 0)
            strzones += QString("FROM BEGIN UNTIL %1, CP=%2:").arg(getEndDate(i).toString("yyyy/MM/dd")).arg(cp);
        else if (i == ranges.size() - 1)
            strzones += QString("FROM %1 UNTIL END, CP=%2:").arg(getStartDate(i).toString("yyyy/MM/dd")).arg(cp);
        else
            strzones += QString("FROM %1 UNTIL %2, CP=%3:").arg(getStartDate(i).toString("yyyy/MM/dd")).arg(getEndDate(i).toString("yyyy/MM/dd")).arg(cp);
	strzones += QString("\n");
	for (int j = 0; j < ranges[i].zones.size(); j ++) {
            const ZoneInfo &zi = ranges[i].zones[j];
	    if (ranges[i].zones[j].hi == INT_MAX)
		strzones += QString("%1,%2,%3,MAX\n").arg(zi.name).arg(zi.desc).arg(zi.lo);
	    else
		strzones += QString("%1,%2,%3,%4\n").arg(zi.name).arg(zi.desc).arg(zi.lo).arg(zi.hi);
        }
	strzones += QString("\n");
        #endif
    }

    QFile file(home.absolutePath() + "/power.zones");
    if (file.open(QFile::WriteOnly))
    {
        QTextStream stream(&file);
        stream << strzones;
        file.close();
    }
}

void Zones::addZoneRange(QDate _start, QDate _end, int _cp)
{
    ranges.append(ZoneRange(_start, _end, _cp));
}

void Zones::addZoneRange()
{
    ranges.append(ZoneRange(date_zero, date_infinity));
}

void Zones::setEndDate(int rnum, QDate endDate)
{
    ranges[rnum].end = endDate;
    modificationTime = QDateTime::currentDateTime();
}
void Zones::setStartDate(int rnum, QDate startDate)
{
    ranges[rnum].begin = startDate;
    modificationTime = QDateTime::currentDateTime();
}

QDate Zones::getStartDate(int rnum) const
{
    assert(rnum >= 0);
    return ranges[rnum].begin;
}

QString Zones::getStartDateString(int rnum) const
{
    assert(rnum >= 0);
    QDate d = ranges[rnum].begin;
    return (d.isNull() ? "BEGIN" : d.toString());
}

QDate Zones::getEndDate(int rnum) const
{
    assert(rnum >= 0);
    return ranges[rnum].end;
}

QString Zones::getEndDateString(int rnum) const
{
    assert(rnum >= 0);
    QDate d = ranges[rnum].end;
    return (d.isNull() ? "END" : d.toString());
}

int Zones::getRangeSize() const
{
    return ranges.size();
}

// generate a zone color with a specific number of zones
QColor zoneColor(int z, int num_zones) {
    assert ((z >= 0) && (z < num_zones));
    if (num_zones == 1)
        return QColor(128, 128, 128);
    QColor color;

    // pick a color from violet (z=0) to red (z=num_zones)
    color.setHsv(int(300 * (num_zones - z - 1) / (num_zones - 1)), 255, 255);

    return color;
}

// delete a range, extend an adjacent (prior if available, otherwise next)
// range to cover the same time period, then return the number of the new range
// covering the date range of the deleted range
int Zones::deleteRange(int rnum) {
    assert((ranges.size() > 1) && (rnum >= 0) && (rnum < ranges.size()));
    int return_rnum;

    // extend an adjacent range
    if (rnum == 0) {
	return_rnum = 0;
	setStartDate(rnum + 1, getStartDate(rnum));
    }
    else {
	return_rnum = rnum - 1;
	setEndDate(return_rnum, getEndDate(rnum));
    }

    // drop higher ranges down one slot
    for (int r = rnum; r < ranges.size() - 1; r ++)
	ranges[r] = ranges[r + 1];

    // reduce the number of ranges by one
    ranges.removeLast();

    return return_rnum;
}

// insert a new range starting at the given date extending to the end of the zone currently
// containing that date.  If the start date of that zone is prior to the specified start
// date, then that zone range is shorted.
int Zones::insertRangeAtDate(QDate date, int cp) {
    assert(date.isValid());
    int rnum;

    if (ranges.empty())
	addZoneRange();
    else {
	rnum = whichRange(date);
	assert(rnum >= 0);
	QDate date1 = getStartDate(rnum);

	// if the old range has dates before the specified, then truncate the old range
	// and shift up the existing ranges
	if (date > date1) {
	    QDate endDate = getEndDate(rnum);
	    setEndDate(rnum, date);
	    ranges.insert(++ rnum, ZoneRange(date, endDate));
	}
    }

    if (cp > 0) {
	setCP(rnum, cp);
	setZonesFromCP(rnum);
    }

    return rnum;
}
