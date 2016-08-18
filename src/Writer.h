#ifndef WRITER_H_
#define WRITER_H_


#include "Control.h"

char *toUnstriped();
char *toStriped();
int readRow(FILE *,int *,int);
char *getFormattedExtension();

#endif /* WRITER_H_ */
