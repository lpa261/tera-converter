#ifndef TC_TEMP_JOB_H
#define TC_TEMP_JOB_H
#include "tc_common.h"
typedef struct TcTempJob TcTempJob;
TcTempJob *tc_temp_job_new(const char *destination,char **error);
const char *tc_temp_job_path(const TcTempJob *job);
const char *tc_temp_job_directory(const TcTempJob *job);
void tc_temp_job_finish(TcTempJob **job);
unsigned tc_temp_job_recover(void);
#endif
