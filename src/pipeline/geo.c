#include "geo.h"

#include <math.h>

#define EARTH_R_M 6371000.0
#define DEG2RAD   (M_PI / 180.0)

double wdm_haversine_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * DEG2RAD;
    double dlon = (lon2 - lon1) * DEG2RAD;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * DEG2RAD) * cos(lat2 * DEG2RAD) *
               sin(dlon / 2) * sin(dlon / 2);
    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return EARTH_R_M * c;
}
