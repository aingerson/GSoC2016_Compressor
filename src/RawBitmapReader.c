#include <stdio.h>
#include <stdlib.h>
#include "Control.h"
#include "Vars.h"
#include "RawBitmapReader.h"
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include "SegUtil.h"
#include "BlockSeg.h"
#include "WAHCompressor.h"
#include "VALCompressor.h"
#include <math.h>
#include "Clock.h"


pthread_t *threads;//thread pointers
char *compressed_folder;//the file location for the compressed files
char col_path[BUFF_SIZE];//the path for the temporary column data files
int next;//next column ready to be compressed (only used for COL_FORMAT)
pthread_mutex_t mut;//mutex for locking threads
int numCols;//number of columns
int extra;//number of files in last striped file (only used for STRIPED)
struct params **toCompress;//only used for OLD STRIPED (weird threading) format -->probably can delete
int size;//number of words in each column
int numFiles;//number of files (only used for STRIPED)
int currFileNum;//current file number we're scanning (only used for STRIPED)
int blockWords;//number of words can scan per block (per thread for STRIPED)
int colNum;//current column number file we're scanning (only used for STRIPED)
int success;//whether we're done readifng all the files (only used for STRIPED)
blockSeg **segs;//structs to hold data to pass to compressor
int id[NUM_THREADS];//threads ids
word_32 *readingBuffer;//buffer to read data into from columns (only used for STRIPED?)

struct params {//struct for thread parameters
	word_32 *words;
	int num;
};

FILE *currFile;
blockSeg **nextCol;//next segment each thread is set to compress (start of each stack)
blockSeg **lastCol;//the segment on the top of the stack




/*
 * Reads a file (given a file name) and compresses it
 * Returns the file extension for every compressed file name
 */
char *readFile(char file[]){
	numCols = 0;
	snprintf(col_path,BUFF_SIZE,"%s%s",file,"/col_");
	//printf("Col_path: %s\n",col_path);
	size = 0;
	pthread_mutex_init(&mut,NULL);
	char *folder = folderName(file);//make a folder for the compressed files
	mkdir(folder,S_IRWXU);
	compressed_folder = (char *) malloc(sizeof(char)*BUFF_SIZE);
	snprintf(compressed_folder,BUFF_SIZE,"%s%s",folder,"/col_");//build the generic file name we used (to return for querying)

	if(RUN_COMPRESSION==0) return compressed_folder;
	int i;
	for(i=0;i<NUM_THREADS;i++){
		id[i]=i;
	}

	runOverhead();

	threads = (pthread_t *) malloc(sizeof(pthread_t) * NUM_THREADS);//allocate each thread pointer

	printf("Start threaded compression\n");

	double start = rtclock();


	if(FORMAT_TYPE == UNSTRIPED)
		compressUnstriped();//start compression
	else if(FORMAT_TYPE == STRIPED)
		compressStriped();

	double end = rtclock();
	printf("End threaded compression\n");
	printf("Compression time: %f\n",(end-start));

	return compressed_folder;
}


/*
 * Runs basic overhead needed to prepare the program for compression
 */
void runOverhead(){
	if(FORMAT_TYPE==UNSTRIPED){
		numCols = 0;
		while(1){
			char *name = getReadingFileName(numCols);//build the column file name
			if(size==0){
				struct stat st;
				stat(name, &st);
				size = (st.st_size)/sizeof(word_32);//this is number of words in each column file (same in each column)
			}
			//counting the number of columns there are in that folder
			if(access(name,F_OK) != -1){//if this file exists
				numCols++;//count it
				continue;//and keep counting
			}
			else{//this file doesn't exit so we know how many columns exist
				break;
			}
		}
		segs = (blockSeg **) malloc(sizeof(blockSeg *) *NUM_THREADS);//pointer for each thread to segment that it's compressing
		next = 0;//tracks which column to compress next (start at the beginning)
		if(CORE==OUT_CORE){
			blockWords = BLOCK_SIZE*1000/sizeof(word_read);//this is how many words we will be scanning every time
			if(COMPRESSION==VAL){
				blockWords -= (blockWords % sizeof(word_32));
			}
			//blockWords -= (blockWords%NUM_THREADS);//(must be divisible by the number of number of threads)
		}
		else{
			blockWords = size;
		}

	}
	else if(FORMAT_TYPE==STRIPED){
		extra = NUM_THREADS-1;//the number of left over columns saved in the last striped file
		numCols=-1;
		while(1){//counting up
			numCols+=NUM_THREADS;
			char *name = getReadingFileName(numCols);//build the file name based on colNum
			if(access(name,F_OK) != -1){//if this file exists
				if(size==0){
					struct stat st;
					stat(name, &st);
					size = (st.st_size)/sizeof(word_32);
					size /= NUM_THREADS;//the number of words in each column
					//TODO: add special case where there are less than NUM_THREADS columns
				}
				continue;//keep counting more files
			}
			else{//this file doesn't exist so we're reaching the end
				break;
			}
		}
		while(1){//counting down
			numCols--;
			char *name = getReadingFileName(numCols);//build the file name based on colNum
			if(access(name,F_OK) != -1){//if this file exists, we've found the last file
				break;
			}
			else{//need to keep going to know how many columns there are
				extra--;//tracks the number of columns saved in the last files
				continue;
			}
		}
		numCols++;
		if(extra==0) extra = NUM_THREADS;
		float n = (float)(numCols);
		n/=(float)(NUM_THREADS);
		numFiles = ceil(n);//figure out the number of striped files we will be scanning
		currFileNum=0;//start the file counter at the beginning
		blockWords = BLOCK_SIZE*1000/sizeof(word_32);//this is how many words we will be scanning every time
		int perCol = blockWords/NUM_THREADS;
		blockWords = perCol*NUM_THREADS;

		readingBuffer = (word_32 *) malloc(sizeof(word_32) * blockWords);//a buffer for reading into
		nextCol = (blockSeg **) malloc(sizeof(blockSeg **) * NUM_THREADS);//pointers for the front of the queue

		if(CORE==OUT_CORE){

			success=1;//tracks whether there are more files left to scan

			lastCol = (blockSeg **) malloc(sizeof(blockSeg **) * NUM_THREADS);//pointers for the end for the queue
			int i;
			for(i=0;i<NUM_THREADS;i++){//initialization for each thread's queue
				nextCol[i] = (blockSeg *) malloc(sizeof(blockSeg));//allocate block segs
				lastCol[i] = (blockSeg *) malloc(sizeof(blockSeg));
				nextCol[i]->next = lastCol[i];//and connect them (like a linked list)
				lastCol[i]->status = NOT_VALID;//neither has any valid data in them
				nextCol[i]->status = NOT_VALID;
			}
		}
		else{
			int i;
			for(i=0;i<NUM_THREADS;i++){//initialization for each thread's queue
				nextCol[i] = (blockSeg *) malloc(sizeof(blockSeg));//allocate block segs
				nextCol[i]->toCompress = (word_read *) malloc(sizeof(word_read)*size);
				nextCol[i]->status = FIRST_LAST;
				nextCol[i]->size = size;
			}
		}

	}
	initUtilSegs();

}

/*
 * Starts compression of the striped files
 */
void compressStriped(){
	if(CORE==OUT_CORE){
		//try reading the first block (different the rest because we're saving into next, not last)
		if(readFirst()==0) return;//if it returned 0, there was nothing in there
		int i;
		for(i=0;i<NUM_THREADS;i++){//start each thread going
			if(pthread_create(&threads[i],NULL,startThread,(void *)(&(id[i])))){
				printf("Error creating thread\n");
				return;
			}
		}
		for(i=0;i<NUM_THREADS;i++){//wait for all the threads to finish
			if(pthread_join(threads[i],NULL)){
				printf("Error joining thread\n");
				return;
			}
		}
	}
	else{

		if(numFiles>1){//normal file
			next=NUM_THREADS-1;
		}
		else if(numFiles>0){//last file (just in case there's only one readable file)
			next=extra-1;
		}
		else{//no files to read (empty)
			return;
		}
		char *name = getReadingFileName(next);//the name of the first file to read
		currFile = fopen(name,"rb");//open it to read
		while(1){

			int thisCols=readStriped();

			int h;
			for(h=0;h<thisCols;h++){//start each thread going
				if(pthread_create(&threads[h],NULL,compressCol,(void *) &(id[h]))){
					printf("Error creating thread\n");
					return;
				}
			}
			for(h=0;h<thisCols;h++){//wait for all the threads to finish
				if(pthread_join(threads[h],NULL)){
					printf("Error joining thread\n");
					return;
				}
			}
			currFileNum++;
			if(currFileNum<numFiles){//there are still more files to read
				if(currFileNum<numFiles-1){//normal next file so the colNum will be NUM_THREADS different
					next+=NUM_THREADS;
				}
				else{//special case-last file (might not have NUM_THREADS columns in it)
					next+=extra;
				}
				char *name = getReadingFileName(next);//the name of the next striped file to open
				currFile = fopen(name,"rb");//open it
			}
			else{//no more files so we're done
				return;
			}
		}
	}
}

/**
 * Reads a block of data from a striped file (returns number of files striped within this file)
 */
int readStriped(){
	int sep = NUM_THREADS;//the number of columns in this file (will probably be NUM_THREADS)
	if(currFileNum==(numFiles-1)){
		sep = extra;//unless it's the last striped file in which case, it might not be
		blockWords = BLOCK_SIZE*1000/sizeof(word_32);
		blockWords /= sep;
		blockWords *= sep;
	}

	int k,m;
	int blockIndex=0;
	int index[sep];
	for(k=0;k<sep;k++){

		index[k]=0;
		nextCol[k]->colFile = fopen(getCompressedFileName(colNum++),"wb");
	}

	while(1){
		int words = fread(readingBuffer,sizeof(word_32),blockWords,currFile);
		int perCol = words/sep;//to write per col

		blockIndex =0;
		for(k=0;k<sep;k++){
			for(m=0;m<perCol;m++){
				nextCol[k]->toCompress[(index[k])++] = readingBuffer[blockIndex++];
			}

		}
		if(words<blockWords) break;
	}


	fclose(currFile);

	return sep;
}

/*
 * Reads the current striped file into lastCols (adds the read data onto the end of each thread's stack)
 */
void *startThread(void *args){
	int *threadNum = (int *) (args);

	//if the block next on the queue is empty (nothing to compress) and there's still more that can be scanned
	if((nextCol[*threadNum]->status == NOT_VALID || nextCol[*threadNum]->status == EMPTY_FIRST) && success !=0){
		pthread_mutex_lock(&mut);//make sure to lock the threads (we don't want to multiple threads to be reading at the same time)

		success=readNext();//read the next and make sure to update the success marker to know if there's more to scan
		pthread_mutex_unlock(&mut);//now the threads can keep compressing
	}
	else if(nextCol[*threadNum]->status == NOT_VALID || nextCol[*threadNum]->status == EMPTY_FIRST){//there's nothing left in my queue to compress
		return NULL;//so I'm done
	}
	prepCompressBlock(*threadNum);//compress whatever is next
	while(success==1 || nextCol[*threadNum]->status != NOT_VALID){

		if((nextCol[*threadNum]->status == NOT_VALID || nextCol[*threadNum]->status == EMPTY_FIRST) && success !=0){
				pthread_mutex_lock(&mut);//make sure to lock the threads (we don't want to multiple threads to be reading at the same time)

				success=readNext();//read the next and make sure to update the success marker to know if there's more to scan
				pthread_mutex_unlock(&mut);//now the threads can keep compressing
			}
			else if(nextCol[*threadNum]->status == NOT_VALID || nextCol[*threadNum]->status == EMPTY_FIRST){//there's nothing left in my queue to compress
				return NULL;//so I'm done
			}
			prepCompressBlock(*threadNum);//compress whatever is next

	}
	return NULL;//otherwise, there's nothing left to do

	return NULL;
}

/*
 * Reads the first block of a striped file (into nextCols instead of lastCols like readNext)
 */
int readFirst(){
	int sep;//the number of columns contained in this file
	if(numFiles>1){//normal file
		colNum=NUM_THREADS-1;
		sep = NUM_THREADS;//full file so has NUM_THREADS files
	}
	else if(numFiles>0){//last file (just in case there's only one readable file)
		colNum=extra-1;
		sep = extra;//last file so might not be full
	}
	else{//no files to read (empty)
		return 0;
	}
	char *name = getReadingFileName(colNum);//the name of the first file to read
	currFile = fopen(name,"rb");//open it to read

	int read = fread(readingBuffer, sizeof(word_32), blockWords, currFile);//transfer over a block from the file to the buffer

	int perCol = read/sep;//the number of words per column (separator)
	int j;//this is the segment we're scanning in this block (which column)
	int k;//word counter for each column
	int m = 0;//word counter for everything we scanned

	for(j=0; j<sep; j++){//go through the number of columns in this block we're scanning
		word_read *temp = (word_read *) malloc(sizeof(word_read) * perCol);//saving array of words into here to save for a column segment
		for(k=0;k<perCol;k++){
			temp[k] = readingBuffer[m++];//transfer all the words for that column
		}

		nextCol[j]->colNum = j;//start the columns at the beginning
		nextCol[j]->toCompress = temp;//save what we just scanned into this block segment
		nextCol[j]->size=perCol;//this is how many words are in this column segment
		lastCol[j]->colNum = j;//next column number is probably this one too (will change later if incorrect)
		lastCol[j]->status=NOT_VALID;//the segment this points to has not been scanned yet
		nextCol[j]->colFile = fopen(getCompressedFileName(j),"wb");//where we're saving the compressed data into

		if(read<blockWords){//getting here means we got to the end of the file
			nextCol[j]->status = FIRST_LAST;//marks this column that we just scanned as both the first and last segment of this column
			lastCol[j]->status=EMPTY_FIRST;//the next one we know will be the first one (but it's also empty)
		}
		else{//otherwise, we still have words to scan in this striped file
			lastCol[j]->status=NOT_VALID;//so the next one is empty
			nextCol[j]->status=READ_FIRST;//and mark this one as being the first word (but not the last)
		}
	}

	if(read<blockWords){//reached the end of file
		fclose(currFile);//we're done with the file we're scanning
		currFileNum++;//increment the file counter so we know we moved on
		if(currFileNum<numFiles){// if there are still more files to read
			for(j=0;j<NUM_THREADS;j++){
				lastCol[j]->colNum = nextCol[j]->colNum + NUM_THREADS;//increment the column number for each thread (because we're moving on)
			}
			if(currFileNum<numFiles-1){//the next file is just another normal file (full columns)
				colNum+=NUM_THREADS;
			}
			else{//special case-last file (might not have NUM_THREADS columns saved in it)
				colNum+=extra;
			}
			char *name = getReadingFileName(colNum);
			currFile = fopen(name,"rb");//open a new reading file
		}
		else{//no more files so we're done
			return 0;
		}
	}

	return 1;
}

/*
 * Reads the next block from the current file
 */
int readNext(){

	if(success==0) return 0;//if we already know we're done scanning all the files to scan
	int read = fread(readingBuffer, sizeof(word_32), blockWords, currFile);//transfer over all the words into the block buffer

	int sep = NUM_THREADS;//the number of columns in this file (will probably be NUM_THREADS)
	if(currFileNum==(numFiles-1)){
		blockWords = BLOCK_SIZE*1000/sizeof(word_32);//this is how many words we will be scanning every time
		blockWords -= (blockWords%sep);//(must be divisible by the number of the number of columns in the file)
		sep = extra;//unless it's the last striped file in which case, it might not be
	}
	int perCol = read/sep;//the number of words per column
	int j;//column/segment tracker
	int k;//word per column tracker
	int m = 0;//tracker for word in reading buffer

	for(j=0; j<sep; j++){//save data for each column in this file
		word_read *temp = (word_read *) malloc(sizeof(word_read) * perCol);//where we're going to be saving this segment's words to compress
		for(k=0;k<perCol;k++){//transfer all the words
			temp[k] = readingBuffer[m++];
		}

		lastCol[j]->toCompress = temp;//set the thread's last segment words to compress
		lastCol[j]->size = perCol;//and save how many words are in there
		if(lastCol[j]->status==EMPTY_FIRST){//if we marked this as the first block of the column
			lastCol[j]->status=READ_FIRST;//still first, but it's read (valid for compressing) now
		}
		else{//otherwise we're just in the middle of the column so it's just valid (not first or last)
			lastCol[j]->status = VALID;
		}

		blockSeg *nextColumn = (blockSeg *) malloc(sizeof(blockSeg));//allocate the pointer for next one
		if(read<blockWords){//we reached the end of the file
			if(lastCol[j]->status == READ_FIRST){//if it was the first block
				lastCol[j]->status = FIRST_LAST;//now mark it as the first AND last
			}
			else{
				lastCol[j]->status = LAST_BLOCK;//mark it as just the last block
			}
			nextColumn->status = EMPTY_FIRST;//we know the next one will have to be a first block (but empty)
			nextColumn->colNum = lastCol[j]->colNum + NUM_THREADS;//the next column won't be the same column number
		}
		else{
			nextColumn->status = NOT_VALID;//the next block is in valid
			nextColumn->colNum = lastCol[j]->colNum;//and it has the same column number
		}
		lastCol[j]->next = nextColumn;//move the pointer of the end of the queue
		lastCol[j] = nextColumn;
	}

	if(read<blockWords){//reached the end of file
		fclose(currFile);//close the file we just scanned
		currFileNum++;//increment the file counter so we know we're moving on
		if(currFileNum<numFiles){//there are still more files to read
			if(currFileNum<numFiles-1){//normal next file so the colNum will be NUM_THREADS different
				colNum+=NUM_THREADS;
			}
			else{//special case-last file (might not have NUM_THREADS columns in it)
				colNum+=extra;
			}
			char *name = getReadingFileName(colNum);//the name of the next striped file to open
			currFile = fopen(name,"rb");//open it

		}
		else{//no more files so we're done
			return 0;
		}
	}

	return 1;//still scanning (more files)
}


/**
 * Deals with getting a block ready to send to the compressor
 */
void prepCompressBlock(int threadNum){

	if(nextCol[threadNum]->colNum>=numCols) return;//this column is not actually a valid column (out of range) so return

	//striped only implemented for WAH, not VAL
	if(COMPRESSION == WAH) compressUsingWAH(nextCol[threadNum]);//send the next block to the compressor
	else return;
	if(nextCol[threadNum]->status==LAST_BLOCK || nextCol[threadNum]->status==FIRST_LAST){//if this happened to be the last one
		fclose(nextCol[threadNum]->colFile);//we can close the file
		if(nextCol[threadNum]->next->colNum<numCols){//and if the next one is in range, open a file for it
			nextCol[threadNum]->next->colFile = fopen(getCompressedFileName(nextCol[threadNum]->next->colNum),"wb");
		}
	}
	else{//not the last file so keep writing to the same file
		nextCol[threadNum]->next->curr = nextCol[threadNum]->curr;
		nextCol[threadNum]->next->colFile = nextCol[threadNum]->colFile;
	}

	blockSeg *temp = nextCol[threadNum];//temp pointer
	nextCol[threadNum] = nextCol[threadNum]->next;//update the top of the queue
	free(temp);//don't need the block we just compressed

}





//UNSTRIPED METHODS


/*
 * Starts compressing the bitmap data after saved in separate column files
 */
void compressUnstriped(){
	int i;
	for(i=0;i<NUM_THREADS;i++){//start each thread going
		segs[i] = (blockSeg *) malloc(sizeof(blockSeg));//allocate the segment pointer
		segs[i]->toCompress = (word_read *) malloc(sizeof(word_read) * blockWords);//allocate the word array buffer in the segment struct
		if(pthread_create(&threads[i],NULL,compressNext,(void *)(&(id[i])))){
			printf("Error creating thread\n");
			return;
		}
	}

	for(i=0;i<NUM_THREADS;i++){//wait for all the threads to finish
		if(pthread_join(threads[i],NULL)){
			printf("Error joining thread\n");
			return;
		}
	}
}

/**
 * Compresses the next column that has not been compressed yet
 */
void *compressNext(void *param){
	int n = -1;//hasn't been assigned a column yet
	int *id = (int *) param;
	pthread_mutex_lock(&mut);//lock everything
	n=(next++);//find out which column we need to compress (and increment for the next thread to compress)
	pthread_mutex_unlock(&mut);//unlock it

	if(n<numCols){//if there's still another column to compress
		compressColumn(n,*id);//go for it
	}
	return NULL;
}



/**
 * Compresses one column (colNum = col)
 */
void compressColumn(int col, int threadNum){
	int runs = 1;
	if(NUM_SEGS==-1) runs=3;
	int i;
	int length = BASE_LEN;
	if(NUM_SEGS>-1) length = (WORD_LENGTH-FLAG_BITS)/NUM_SEGS;

	unsigned int min = 0;
	min--;
	int optimal=0;

	for(i=0;i<runs;i++){
		FILE *ptr = fopen(getReadingFileName(col),"rb");//open the uncompressed file
		int read=blockWords;//will hold how many words have been successfully read

		segs[threadNum]->colNum = col;//save colNumber of this block
		if(CORE==OUT_CORE) segs[threadNum]->status=READ_FIRST;//we know it's the first one
		else segs[threadNum]->status = FIRST_LAST;

		segs[threadNum]->colFile = fopen(getCompressedFileName(col),"wb");//open the respective file for writing
		int numWords = 0;

		while(read==blockWords){//while we still can read

			read = fread(segs[threadNum]->toCompress, sizeof(word_read), blockWords, ptr);//transfer over all the words
			if(read<blockWords){
				if(segs[threadNum]->status==READ_FIRST) segs[threadNum]->status=FIRST_LAST;
				else segs[threadNum]->status = LAST_BLOCK;
			}
			segs[threadNum]->size = read;//and save how many words there are in there

			if(COMPRESSION==WAH) compressUsingWAH(segs[threadNum]);//compress it
			else if(COMPRESSION==VAL) numWords += compressUsingVAL(segs[threadNum],length);
			if(CORE==IN_CORE) break;
			segs[threadNum]->status = VALID;//not at the beginning anymore
			if(read<blockWords){//if we reached the end of the file
				fclose(segs[threadNum]->colFile);//close the compressed file
				break;//and leave
			}
		}
		fclose(ptr);//close the uncompressed column data file

		if(COMPRESSION==VAL && NUM_SEGS==-1){
			if(numWords<min){//if the column we just compressed is smaller than a previous max
				optimal = length;
				min=numWords;//save it as the new minimum size
				remove(getCompressedFileName(numCols+threadNum));//delete whatever we were saving there before
				rename(getCompressedFileName(col),getCompressedFileName(numCols));//save min column there for later
			}
			length+=BASE_LEN;
		}
	}
	if(COMPRESSION==VAL && NUM_SEGS==-1){
		rename(getCompressedFileName(numCols+threadNum),getCompressedFileName(col));//save the smallest column to be where we want it
	}
	compressNext(&threadNum);//try to compress more
}


//Helper methods dealing with folder names and extensions

/**
 * Returns the path of the file
 * Ex. file = "Files/data.txt"
 * Returns "Files/"
 */
char *getPath(char file[]){
	int length;
	for(length=0;file[length]!='\0';length++);
	int i;
	for(i=length-1;file[i]!='/';i--);
	length=i;

	char *ret = (char *) malloc(sizeof(char)*(length+1));

	for(i=0;i<length;i++){
		ret[i] = file[i];
	}
	ret[i] = '\0';

	return ret;
}

/**
 * Returns the directory name in the same place as the file with name folder.
 * Ex. file = "Files/data.txt"		folder = "compressed_WAH32_"
 * Returns "Files/compressed_WAH32_data.txt"
 */
char *getDir(char file[], char folder[]){

	int file_length,folder_length;
	for(file_length=0;file[file_length]!='\0';file_length++);
	for(folder_length=0;folder[folder_length]!='\0';folder_length++);

	int backslash;
	for(backslash=file_length;file[backslash]!='/';backslash--);

	char *ret = (char *) malloc(sizeof(char) * (file_length+folder_length+2));
	int i;
	for(i=0;i<=backslash;i++){
		ret[i] = file[i];
	}
	int j;
	for(j=0;j<folder_length;ret[i++]=folder[j++]);
	for(j=backslash+1;j<file_length;ret[i++]=file[j++]);

	ret[i]='\0';


	return ret;
}



/**
 * Returns the appropriate file path for the compressed files (based on the type of compression)
 * Example: file = "Files/data.txt" and using WAH32 compression
 * Returns: "Files/compressed_WAH32_data.txt"
 */
char *folderName(char file[]){
	char *ret;
	if(COMPRESSION == WAH && WORD_LENGTH==32){
		ret = getDir(file,"compressed_WAH32_");
	}
	else if(COMPRESSION == WAH && WORD_LENGTH==64){
		ret = getDir(file,"compressed_WAH64_");
	}
	else if(COMPRESSION == VAL && WORD_LENGTH==32){
		ret = getDir(file,"compressed_VAL32_");
	}
	else if(COMPRESSION == VAL && WORD_LENGTH==64){
		ret = getDir(file,"compressed_VAL64_");
	}
	return ret;
}



/**
 * Returns the name of a compressed file name (according to extension in compressed_folder) of column number col
 */
char *getCompressedFileName(int col){
	char *column = (char *) malloc(sizeof(char) * BUFF_SIZE);
	snprintf(column,BUFF_SIZE,"%s%d.dat",compressed_folder,col);//this is where we will save the compressed column
	return column;
}

/**
 * Returns the name of a UNcompressed file name (according to extension in col_path) of column number col
 */
char *getReadingFileName(int col){
	char *column = (char *) malloc(sizeof(char) * BUFF_SIZE);
	snprintf(column, BUFF_SIZE, "%s%d.dat",col_path,col);//this is the uncompressed file name
	return column;
}
