#define _CRT_SECURE_NO_WARNINGS
#include <stdlib.h>
#include <string.h>
#include "tf_port.h"

/* The simulator has no gauge, so the reading comes from the environment. That keeps every
   state the UI can show reachable from a screenshot run:
     TF_BATT=87      three green bars
     TF_BATT=40      two yellow bars
     TF_BATT=10      one red bar
     TF_BATT=40C     charging (yellow bolt)
     TF_BATT=100F    full (green bolt)
     unset or "none" no gauge at all, widget hidden */
bool tf_port_battery(tf_batt_t *out)
{
    const char *e = getenv("TF_BATT");
    if (!e || !*e || !strcmp(e, "none")) return false;

    const char last = e[strlen(e) - 1];
    out->charging = last == 'C' || last == 'F';
    out->full = last == 'F';

    int percent = 0;
    for (; *e >= '0' && *e <= '9'; e++) percent = percent * 10 + (*e - '0');
    out->percent = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    return true;
}
