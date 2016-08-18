
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "RawBitmapReader.h"
#include <string.h>
#include <time.h>
#include "Query.h"
#include "Writer.h"
#include "Clock.h"
#include "VALQuery.h"

/**
 * Runs formatter/compressor/query engines as set in Control.h
 */
int main() {

	setbuf(stdout,NULL);

	if(RUN_FORMAT){
		char *folder;
		if(FORMAT_TYPE == UNSTRIPED)
			folder = toUnstriped();
		else if(FORMAT_TYPE == STRIPED)
			folder = toStriped();
		printf("Reformatted file extension\t%s\n",folder);
		return 1;
	}

	char *newFile;//will hold where the reformatted files are
	newFile=getFormattedExtension();

	double start,end;//start and end clocking times
	double total;//total amount of time compression ran
	char *folder;


	if(RUN_COMPRESSION){
		char *format;
		char *comp;
		if(COMPRESSION==WAH) comp = "WAH";
		else if(COMPRESSION==VAL) comp = "VAL";
		if(FORMAT_TYPE==UNSTRIPED) format = "UNSTRIPED";
		else if(FORMAT_TYPE==STRIPED) format = "STRIPED";
		printf("Start compression\t%s\t%s\tLength=%d\t%dkB\t%d thread(s)\t%s\n",BITMAP_FILE,comp,WORD_LENGTH,BLOCK_SIZE,NUM_THREADS,format);
		start = rtclock();
		folder = readFile(newFile);
		end = rtclock();
		total = (end-start);
		printf("End compression\n");
		printf("Total compression time:\t%f\n",total);
		return 1;
	}
	else{
		folder = readFile(newFile);
	}

	if(RUN_QUERY){
		printf("Start query of %s\t%d threads\n",folder,NUM_THREADS);
		start = clock();
		runQueries(folder);
		end = clock();
		total = (end-start)/CONV;
		printf("End querying\n");
		printf("Total query time: %f\n",total);
	}

	return 0;
}
