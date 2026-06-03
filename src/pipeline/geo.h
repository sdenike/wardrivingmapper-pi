/**
 * geo.h — great-circle distance for position-aware dedup.
 *
 * The scanner suppresses re-logging an AP seen within DEDUP_RADIUS_M of where
 * it was last logged (firmware parity, 75 m), so driving the same loop yields
 * one record per AP per loop instead of thousands.
 */
#pragma once

#define DEDUP_RADIUS_M 75.0

/* Distance in metres between two WGS-84 points. */
double wdm_haversine_m(double lat1, double lon1, double lat2, double lon2);
