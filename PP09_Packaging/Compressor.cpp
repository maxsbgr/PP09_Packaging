#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <ShlObj.h>
#include <windows.h>
#include <ctime>
#include <string>
#include <vector>
#include <iterator>
#include "Shlwapi.h"


// For combining compressed files to a single zip-archive
#include "zip.h"  // source: http://www.codeproject.com/Articles/7530/Zip-Utils-clean-elegant-simple-C-Win
#include "unzip.h"

#include "lz4.h"
#include "wfLZ.h"
#include "quicklz.h"
#include "snappy-c.h"

using namespace std;

#ifndef NULL
#define NULL 0
#endif

#define _SECOND ((int64) 10000000)
#define _MINUTE (60 * _SECOND)
#define _HOUR   (60 * _MINUTE)
#define _DAY    (24 * _HOUR)

// Flag to set bench file header
boolean isBenchFilePrepared = false;
// Total elapsed time of compression and decompression for a compression method
int totalTime = 0;

// For storing system time
LPSYSTEMTIME startTime = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));
LPSYSTEMTIME endTime = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));
// For storing the chosen path
char PATH[MAX_PATH];

// Names for bench file and compressed/decompressed files folders
string BENCH_FILE = "...Results.csv";
string COMPRESSED_FOLDER = "...Compressed Files";
string DECOMPRESSED_FOLDER = "...Decompressed Files";

// Structure for storing LZ4 chunk parameters
struct lz4chunkParameters
{
	int   id;
	char* origBuffer;
	char* compBuffer;
	int   origSize;
	int   compSize;
};

// Set the header for the bench file
void prepareBenchFile(string parentPath) {
	if (!isBenchFilePrepared) {
		// Init the bench file
		ofstream benchFile;
		benchFile.open(parentPath + "\\" + BENCH_FILE, std::ios_base::app);
		vector<string> benchbuffer(1);

		// Set the header
		benchbuffer.push_back("File Name;");
		benchbuffer.push_back("Compression Time;");
		benchbuffer.push_back("Decompression Time;");
		benchbuffer.push_back("Size before compression;");
		benchbuffer.push_back("Size after compression;");
		benchbuffer.push_back("Compression Ratio;\n");

		// Write to file
		for (unsigned int i = 0; i < benchbuffer.size(); i++) {
			benchFile << benchbuffer[i];
		}

		// Finish
		benchbuffer.clear();
		isBenchFilePrepared = true;
	}
}

// Method to add compression and decompression info to the bench file after the process is done
void addToBenchFile(string parentPath, bool isHeaderSet, string compMethod, string fileName, int compTime, 
	int decompTime, int origSize, int compSize) {

	// Init the bench file
	ofstream benchfile;
	benchfile.open(parentPath + "\\" + BENCH_FILE, std::ios_base::app);
	vector<string> benchbuffer(1);

	// Set the sub header
	if (!isHeaderSet) {
		benchbuffer.push_back("--- " + compMethod + " ---;---;---;---;---;---;\n");
	}		
	// Add the process details
	benchbuffer.push_back(fileName + ";");
	benchbuffer.push_back(to_string(compTime) + " ms;");
	benchbuffer.push_back(to_string(decompTime) + " ms;");
	// Convert from byte to kilobyte
	benchbuffer.push_back(to_string((int) ceil((float) origSize / 1024)) + " KB;");
	benchbuffer.push_back(to_string((int)ceil((float)compSize / 1024)) + " KB;");

	benchbuffer.push_back(to_string((float) compSize / (float) origSize) + ";\n");

	for (unsigned int i = 0; i < benchbuffer.size(); i++) {
		benchfile << benchbuffer[i];
	}
	// end of benchmark
	benchbuffer.clear();

}

// Source: http://www.codeproject.com/Articles/2604/Browse-Folder-dialog-search-folder-and-all-sub-fol
// with slight modifications
void SearchFolder(string path, vector<string> *files) {

	//Declare all needed handles     
	WIN32_FIND_DATA FindFileData;
	HANDLE hFind;

	string pathBackup;

	//Make a backup of the directory the user chose         
	pathBackup.assign(path);

	//Find the first file in the directory the user chose     
	hFind = FindFirstFile("*.*", &FindFileData);

	//Use a do/while so we process whatever FindFirstFile returned     
	do
	{
		//Is it valid?         
		if (hFind != INVALID_HANDLE_VALUE)
		{
			//Is it a . or .. directory? If it is, skip, or we'll go forever.             
			if (!(strcmp(FindFileData.cFileName, ".")) ||
				!(strcmp(FindFileData.cFileName, ".."))) {
				continue;
			}
			//Restore the original directory chosen by the user             
			path = pathBackup;
			//Append the file found on to the path of the direcory the user chose
			path.append("\\");
			path.append(FindFileData.cFileName);

			/* If SetCurrentDirectory Succeeds ( returns 1 ) the current file is a directory. Pause this function, 
			and have it call itself. This will begin the whole process over in a sub directory. */
			if ((SetCurrentDirectory(path.data())))
			{
				string folder = path.substr(path.find_last_of("\\") + 1);
				if (folder != COMPRESSED_FOLDER && folder != DECOMPRESSED_FOLDER) {
					SearchFolder(path, files);
				}

			}
			else {
				// add filename to file-vector
				string file = path.substr(path.find_last_of("\\") + 1);
				if (file != BENCH_FILE && file.substr(file.find_last_of(".")) != ".zip") {
					files->push_back(path);
				}
			}
		}
	} while (FindNextFile(hFind, &FindFileData)
		&& hFind != INVALID_HANDLE_VALUE);
	FindClose(hFind);
}

// Select the folder with the chosen path
vector<string> selectFolder(char *path) {
	vector<string> filePaths;

	BROWSEINFO bi = { 0 };
	bi.lpszTitle = "Select folder for compression.";
	LPITEMIDLIST pidl = SHBrowseForFolder(&bi);

	if (pidl != 0) {
		SHGetPathFromIDList(pidl, path);

		SetCurrentDirectory(path);

		SearchFolder(path, &filePaths);

		IMalloc * imalloc = 0;
		if (SUCCEEDED(SHGetMalloc(&imalloc))) {
			imalloc->Free(pidl);
			imalloc->Release();
		}
	}

	return filePaths;
}

// Compress the chosen files with LZ4 compression method and write the process details to the bench file
void compressWithLZ4(vector<string> filePaths, string parentPath) {

	// Chunk size in bytes
	const int chunkSize = 65536;
	// If the sub header for this process is set
	bool isHeaderSet = false;

	// Loop through the files
	for (unsigned int i = 0; i < filePaths.size(); i++) {

		// Variables to be used later
		FILE* origFile = NULL;
		FILE* compFile = NULL;
		FILE* decompFile = NULL;

		const char* origFilePath = filePaths[i].c_str();

		// Set the names and paths of the compressed and decompressed files
		string compFileName;
		const size_t last_slash_idx = filePaths[i].rfind('\\');
		if (std::string::npos != last_slash_idx) {
			compFileName = filePaths[i].substr(last_slash_idx + 1, filePaths[i].length());
		}
		string origFileName = compFileName;
		compFileName.append("_compressed.lz4");
		string compStr = parentPath + "\\" + COMPRESSED_FOLDER + "\\" + compFileName;
		const char* compFilePath = compStr.c_str();
		string decompStr = parentPath + "\\" + DECOMPRESSED_FOLDER + "\\" + origFileName;
		const char* decompFilePath = decompStr.c_str();

		// Prepare the read and write buffers
		char* inBuffer;
		char* outBuffer;
		int numRead = 0;
		unsigned long totalRead = 0, totalWrote = 0;
		int numChunks = 0;
		fopen_s(&origFile, origFilePath, "rb");
		fopen_s(&compFile, compFilePath, "wb");

		if (!compFile || !origFile) {
			printf("Couldn't open file!\n");
			continue;
		}
		struct stat buf;
		stat(origFilePath, &buf);
		int fileSize = buf.st_size;
		// Allocate
		struct lz4chunkParameters* chunkP;
		chunkP = (struct lz4chunkParameters*) malloc(((fileSize / (size_t)chunkSize) + 1) * sizeof(struct lz4chunkParameters));
		inBuffer = (char*)malloc((size_t)fileSize);
		numChunks = (int)((int)fileSize / chunkSize) + 1;
		int maxCompressedChunkSize = LZ4_compressBound(chunkSize);
		int compressedBuffSize = numChunks * maxCompressedChunkSize;
		outBuffer = (char*)malloc((size_t)compressedBuffSize);



		// Init chunks data
		{
			size_t remaining = fileSize;
			char* in = inBuffer;
			char* out = outBuffer;
			for (int i = 0; i < numChunks; i++)
			{
				chunkP[i].id = i;
				chunkP[i].origBuffer = in; in += chunkSize;
				if ((int)remaining > chunkSize) { chunkP[i].origSize = chunkSize; remaining -= chunkSize; }
				else { chunkP[i].origSize = (int)remaining; remaining = 0; }
				chunkP[i].compBuffer = out; out += maxCompressedChunkSize;
				chunkP[i].compSize = 0;
			}
		}

		int readSize = fread(inBuffer, 1, fileSize, origFile);
		fclose(origFile);

		if (readSize != fileSize)
		{
			printf("\nError: problem reading file '%s' !!    \n", origFilePath);
			free(inBuffer);
			free(outBuffer);
			free(chunkP);
			continue;
		}
		
		// Compression
		GetSystemTime(startTime);

		for (int i = 0; i < fileSize; i++) outBuffer[i] = (char)i;     // warmimg up memory

		for (int chunk = 0; chunk < numChunks; chunk++) {
			chunkP[chunk].compSize = LZ4_compress_fast(chunkP[chunk].origBuffer, chunkP[chunk].compBuffer, 
				chunkP[chunk].origSize, maxCompressedChunkSize, 2);
		}
		int compFileSize = 0;
		for (int i = 0; i < numChunks; i++) {
			compFileSize += chunkP[i].compSize;
		}
		char* out = outBuffer;
		fwrite(out, 1, compFileSize, compFile);
		fclose(compFile);

		GetSystemTime(endTime);
		// End compression

		// Compression time in miliseconds
		int compTime = 1000 * (endTime->wSecond - startTime->wSecond) + endTime->wMilliseconds - startTime->wMilliseconds;
		totalTime += compTime;

		// Swap buffers
		char* tmp = inBuffer;
		inBuffer = outBuffer;
		outBuffer = tmp;
		fopen_s(&compFile, compFilePath, "rb");
		readSize = fread(inBuffer, 1, compFileSize, compFile);
		fclose(compFile);

		if (readSize != compFileSize)
		{
			printf("\nError: problem reading file '%s' !!    \n", origFilePath);
			free(inBuffer);
			free(outBuffer);
			free(chunkP);
			continue;
		}

		// Decompression
		GetSystemTime(startTime);

		for (int chunk = 0; chunk < numChunks; chunk++) {
			LZ4_decompress_fast(chunkP[chunk].compBuffer, chunkP[chunk].origBuffer, chunkP[chunk].origSize);
		}

		GetSystemTime(endTime);
		// End decompression

		// Decompression time in miliseconds
		int decompTime = 1000 * (endTime->wSecond - startTime->wSecond) + endTime->wMilliseconds - startTime->wMilliseconds;
		totalTime += decompTime;

		// Write decompressed files
		//fopen_s(&decompFile, decompFilePath, "wb");
		//if (!decompFile) {
		//	printf("Couldn't open file %s!\n", decompFilePath);
		//	continue;
		//}
		//fwrite(outbuffer, 1, fileSize, decompFile);
		//fclose(decompFile);

		//printf("Decompression finished!\n");
		// End of decompression

		// Write benchmark data to csv-file
		addToBenchFile(parentPath, isHeaderSet, "LZ4", origFileName, compTime, decompTime, 
			fileSize, compFileSize);
		isHeaderSet = true;

		// cleanup
		free(inBuffer);
		free(outBuffer);
	}
}

// Helper Malloc function
void* Malloc(const size_t size)
{
	return malloc(size);
	return malloc((size + 3)&(~0x3)); // assume sane allocators pad allocations to four bytes
}

// Source: https://github.com/ShaneYCG/wflz/blob/master/example/main.c
// with modifications
// Compress the chosen files with wfLZ compression method and write the process details to the bench file
void compressWithWFLZ(vector<string> filePaths, string parentPath) {

	// Variables necessary for the process
	uint32_t compSize, origSize;
	uint8_t *origMem, *compMem, *workMem;
	errno_t err;

	// If the sub header for this process is set
	bool isHeaderSet = false;

	// Loop through the files
	for (unsigned int i = 0; i < filePaths.size(); i++) {
		{
			// Read the original file and ready the memories
			const char* origFilePath = filePaths[i].c_str();
			FILE* fh;
			err = fopen_s(&fh, origFilePath, "rb");
			if (fh == NULL) {
				cout << "Could not open file " << origFilePath << endl;
				continue;
			}
			fseek(fh, 0, SEEK_END);
			origSize = (uint32_t)ftell(fh);
			if (origSize == 0) {
				cout << "Empty file " << i << endl << endl;
				continue;
			}
			origMem = (uint8_t*)Malloc(origSize);
			if (origMem == NULL) {
				fclose(fh);
				cout << "Error: Allocation failed.\n" << endl;
				continue;
			}
			fseek(fh, 0, SEEK_SET);
			if (fread(origMem, 1, origSize, fh) != origSize) {
				fclose(fh);
				cout << "Error: File read failed.\n" << endl;
				continue;
			}
			fclose(fh);
		}
		workMem = (uint8_t*)Malloc(wfLZ_GetWorkMemSize());
		if (workMem == NULL)
		{
			cout << "Error: Allocation failed.\n" << endl;
			continue;
		}

		// Compression
		GetSystemTime(startTime);

		compMem = (uint8_t*)Malloc(wfLZ_GetMaxCompressedSize(origSize));
		compSize = wfLZ_CompressFast(origMem, origSize, compMem, workMem, 0);

		GetSystemTime(endTime);
		// End compression

		// Compression time
		int compTime = 1000 * (endTime->wSecond - startTime->wSecond) + endTime->wMilliseconds - startTime->wMilliseconds;
		totalTime += compTime;

		// Ready the compressed file
		ofstream compFile;
		string origFilePath = filePaths[i];

		// Set the compressed file name
		string compFileName;
		const size_t last_slash_idx = filePaths[i].rfind('\\');
		if (std::string::npos != last_slash_idx) {
			compFileName = origFilePath.substr(last_slash_idx + 1, origFilePath.length());
		}
		// Set the original file name mid-process
		string origFileName = compFileName;
		compFileName.append("_compressed.wflz");

		// Fill the compressed file
		compFile.open(parentPath + "\\" + COMPRESSED_FOLDER + "\\" + compFileName, std::ios::binary | std::ios::ate);
		const char* temp = (char*)compMem;
		compFile.write(temp, compSize);
		compFile.close();

		// Decompression
		GetSystemTime(startTime);
		wfLZ_Decompress(compMem, origMem);
		GetSystemTime(endTime);

		// Decompression time
		int decompTime = 1000 * (endTime->wSecond - startTime->wSecond) + endTime->wMilliseconds - startTime->wMilliseconds;
		totalTime += decompTime;

		// Add the details to the bencht file
		addToBenchFile(parentPath, isHeaderSet, "wfLZ", origFileName, compTime, decompTime, (int)origSize, 
			(int)compSize);
		isHeaderSet = true;

		// Clean up
		free(workMem);
		free(compMem);
		free(origMem);
	}
}

// QuickLZ (Replacement of Pithy due to errors in code and lack of documentation)
void compressWithQuickLZ(vector<string> filepaths, string parentPath) {

	uint32_t compressedSize, uncompressedSize;
	errno_t err;

	// Optional Zip File - Initialization
	string zfstr = parentPath + "\\" + "compressedFiles.zip";
	const TCHAR* zipfile = zfstr.c_str();
	HZIP zipArchive = CreateZip(zipfile, 0, 0); // Creates new zip file

												// Header for bench file
	vector<string> benchbuffer(1);
	benchbuffer.push_back("--- QuickLZ ---;---;---;\n");

	for (unsigned int i = 0; i < filepaths.size(); i++) {

		char* uncompressed;
		char* compressed;
		const char* uncompFileName;

		{
			uncompFileName = filepaths[i].c_str();
			FILE* fh;
			err = fopen_s(&fh, uncompFileName, "rb");
			if (fh == NULL)
			{
				cout << "Could not open file " << uncompFileName << endl;
				continue;
			}
			fseek(fh, 0, SEEK_END);
			uncompressedSize = (uint32_t)ftell(fh);
			if (uncompressedSize == 0) {
				cout << "Empty file " << i << endl << endl;
				continue;
			}
			uncompressed = (char*)Malloc(uncompressedSize);
			if (uncompressed == NULL)
			{
				fclose(fh);
				cout << "Error: Allocation failed.\n" << endl;
				continue;
			}
			fseek(fh, 0, SEEK_SET);
			if (fread(uncompressed, 1, uncompressedSize, fh) != uncompressedSize)
			{
				fclose(fh);
				cout << "Error: File read failed.\n" << endl;
				continue;
			}
			fclose(fh);
		}

		qlz_state_compress* state_compress = (qlz_state_compress *)malloc(sizeof(qlz_state_compress));
		compressed = (char*)Malloc(uncompressedSize + 400);

		LPSYSTEMTIME start = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));
		LPSYSTEMTIME end = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));

		GetSystemTime(start);
		// compress file
		compressedSize = qlz_compress(uncompressed, compressed, uncompressedSize, state_compress);

		GetSystemTime(end);

		int compTime = 1000 * (end->wSecond - start->wSecond) + end->wMilliseconds - start->wMilliseconds;
		totalTime += compTime;

		ofstream outputfile;
		string outputfilename;
		outputfilename = filepaths[i];

		outputfilename.substr();

		string compressedFileName;
		const size_t last_slash_idx = outputfilename.rfind('\\');
		if (std::string::npos != last_slash_idx) {
			compressedFileName = outputfilename.substr(last_slash_idx + 1, outputfilename.length());
		}
		// string restored = compressedFileName; // for writing decompressed file
		compressedFileName.append("_compressed.qlz");

		outputfile.open(parentPath + "\\" + COMPRESSED_FOLDER + "\\" + compressedFileName, std::ios::binary | std::ios::ate);
		const char* temp = compressed;
		outputfile.write(temp, compressedSize);

		// Optional Zip File - Add file
		ZipAdd(zipArchive, compressedFileName.c_str(), (TCHAR*)compressed, compressedSize);

		outputfile.close();
		// Decompression QuickLZ
		qlz_state_decompress *state_decompress = (qlz_state_decompress *)malloc(sizeof(qlz_state_decompress));
		unsigned int decompressedSize = qlz_size_decompressed(compressed);
		char* decompressed = (char*)malloc(decompressedSize);

		GetSystemTime(start);
		decompressedSize = qlz_decompress(compressed, decompressed, state_decompress);
		GetSystemTime(end);

		int decompTime = 1000 * (end->wSecond - start->wSecond) + end->wMilliseconds - start->wMilliseconds;
		totalTime += decompTime;

		/* write decompressed file
		compFile.open(parentPath + "\\" + restored, std::ios::binary | std::ios::ate);
		temp = decompressed;
		compFile.write(temp, decompressedSize);
		*/


		ofstream benchfile;
		benchfile.open(parentPath + "\\" + BENCH_FILE, std::ios_base::app);

		// Write benchmark data to csv-file
		benchbuffer.push_back(compressedFileName + ";");
		benchbuffer.push_back(to_string(decompTime) + " ms;");
		benchbuffer.push_back(to_string((float)compressedSize / (float)uncompressedSize) + ";\n");

		for (unsigned int i = 0; i < benchbuffer.size(); i++) {
			benchfile << benchbuffer[i];
		}

		free(state_compress);
		free(state_decompress);
		free(compressed);
		free(decompressed);
		free(uncompressed);
		benchbuffer.clear();
	}
	// Optional Zip File - Close Archive
	cout << "ZIP file is: " << zipfile << endl;
	CloseZip(zipArchive);
}

// source: https://snappy.angeloflogic.com/downloads/
void compressWithSNAPPY(vector<string> filepaths, string parentPath) {
	uint32_t compressedSize, uncompressedSize;
	errno_t err;

	// Optional Zip File - Initialization
	//string zfstr = parentPath + "\\" + "compressedFiles.zip";
	//const TCHAR* zipfile = zfstr.c_str();
	//cout << "ZIP: " << zipfile << endl;
	//HZIP zipArchive = CreateZip(zipfile, 0, 0); // Creates new zip file

	// Header for bench file
	vector<string> benchbuffer(1);
	benchbuffer.push_back("--- Snappy ---;---;---;\n");

	for (unsigned int i = 0; i < filepaths.size(); i++) {

		char* uncompressed;
		char* compressed;
		const char* uncompFileName;

		{
			uncompFileName = filepaths[i].c_str();
			FILE* fh;
			err = fopen_s(&fh, uncompFileName, "rb");
			if (fh == NULL)
			{
				cout << "Could not open file " << uncompFileName << endl;
				continue;
			}
			fseek(fh, 0, SEEK_END);
			uncompressedSize = (uint32_t)ftell(fh);
			if (uncompressedSize == 0) {
				cout << "Empty file " << i << endl << endl;
				continue;
			}
			uncompressed = (char*)Malloc(uncompressedSize);
			if (uncompressed == NULL)
			{
				fclose(fh);
				cout << "Error: Allocation failed.\n" << endl;
				continue;
			}
			fseek(fh, 0, SEEK_SET);
			if (fread(uncompressed, 1, uncompressedSize, fh) != uncompressedSize)
			{
				fclose(fh);
				cout << "Error: File read failed.\n" << endl;
				continue;
			}
			fclose(fh);
		}

		compressedSize = snappy_max_compressed_length(uncompressedSize);
		compressed = new char[compressedSize];

		// compress file
		snappy_status comp_status = snappy_compress(uncompressed, uncompressedSize, compressed, &compressedSize);

		ofstream outputfile;
		string outputfilename;
		outputfilename = filepaths[i];

		outputfilename.substr();

		string compressedFileName;
		const size_t last_slash_idx = outputfilename.rfind('\\');
		if (std::string::npos != last_slash_idx) {
			compressedFileName = outputfilename.substr(last_slash_idx + 1, outputfilename.length());
		}
		string restored = compressedFileName; // for writing decompressed file
		compressedFileName.append("_compressed.snappy");

		outputfile.open(parentPath + "\\" + COMPRESSED_FOLDER + "\\" + compressedFileName, std::ios::binary | std::ios::ate);
		const char* temp = compressed;
		outputfile.write(temp, compressedSize);

		// Optional Zip File - Add file
		//ZipAdd(zipArchive, compressedFileName.c_str(), (TCHAR*)compressed, compressedSize);

		outputfile.close();
		// Decompression QuickLZ
		uint32_t decompressedSize;
		snappy_status size_status = snappy_uncompressed_length(
			compressed, compressedSize, &decompressedSize);

		char* decompressed = (char*)malloc(decompressedSize);

		LPSYSTEMTIME start = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));
		LPSYSTEMTIME end = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));

		GetSystemTime(start);
		snappy_status status = snappy_uncompress(compressed, compressedSize, decompressed, &decompressedSize);
		GetSystemTime(end);

		int elapsedTime = 1000 * (end->wSecond - start->wSecond) + end->wMilliseconds - start->wMilliseconds;
		totalTime += elapsedTime;

		/* write decompressed file*/
		outputfile.open(parentPath + "\\" + restored, std::ios::binary | std::ios::ate);
		temp = decompressed;
		outputfile.write(temp, decompressedSize);



		ofstream benchfile;
		benchfile.open(parentPath + "\\" + BENCH_FILE, std::ios_base::app);

		// Write benchmark data to csv-file
		benchbuffer.push_back(compressedFileName + ";");
		benchbuffer.push_back(to_string(elapsedTime) + " ms;");
		benchbuffer.push_back(to_string((float)compressedSize / (float)uncompressedSize) + ";\n");

		for (unsigned int i = 0; i < benchbuffer.size(); i++) {
			benchfile << benchbuffer[i];
		}

		free(compressed);
		free(decompressed);
		free(uncompressed);
		benchbuffer.clear();
	}
	// Optional Zip File - Close Archive
	//CloseZip(zipArchive);
}

void afterCompressing(string compressionMethod) {
	cout << "Compressing with " << compressionMethod << " complete! That took " << totalTime << " ms." << endl
		<< endl << "See the " << BENCH_FILE << " file for details." << endl;
	cout << "Do you want to open the directory? (y / n)" << endl;

	totalTime = 0;
	bool exit = false;
	while (!exit) {
		char mode = getchar();
		switch (mode) {
		case 'Y':
		case 'y':
			ShellExecute(NULL, "open", PATH, NULL, NULL, SW_SHOWDEFAULT);
			exit = true;
			break;
		case 'N':
		case 'n':
			exit = true;
			break;
		default:
			break;
		}
	}

	cout << endl << "Please select algorithm: " << endl << "Press key 1 for LZ4" << endl <<
		"Press key 2 for wfLZ" << endl << "Press key 3 for QuickLZ" << endl << "Press key 4 for Snappy" << endl <<
		"Press key 5 for all of the above" << endl << "Press key e key to exit program" << endl;
}

int main(void) {

	cout << "Please select folder to compress" << endl;
	vector<string> filepaths;
	filepaths = selectFolder(PATH);

	cout << "Folder selected: " << PATH << endl << endl;

	cout << "Please select algorithm: " << endl << "Press key 1 for LZ4" << endl <<
		"Press key 2 for wfLZ" << endl << "Press key 3 for QuickLZ" << endl << "Press key 4 for Snappy" << endl <<
		"Press key 5 for all of the above" << endl << "Press key e key to exit program" << endl;

	// This makes the user only able to open the file in read-only mode while the program is still running
	// Otherwise benchmark doesn't write while the file is open
	string benchName = (string)PATH + "\\" + BENCH_FILE;
	remove(benchName.c_str());
	ofstream benchfile;
	benchfile.open(benchName, std::ios_base::app);

	// Create output directories
	if (!((CreateDirectory(string((string)PATH + "\\" + COMPRESSED_FOLDER + "\\").c_str(), NULL) ||
		ERROR_ALREADY_EXISTS == GetLastError()) &&
		(CreateDirectory(string((string)PATH + "\\" + DECOMPRESSED_FOLDER + "\\").c_str(), NULL) ||
		ERROR_ALREADY_EXISTS == GetLastError()))) {
		cerr << "Error: Failed to create output directories." << endl;
		return -1;
	}

	char mode = 0;

	while (true) {

		mode = getchar();

		cout << endl;
		switch (mode) {
		case '1':
			cout << "Compressing files with LZ4..." << endl;
			prepareBenchFile(PATH);
			compressWithLZ4(filepaths, PATH);
			afterCompressing("LZ4");
			break;
		case '2':
			cout << "Compressing files with wfLZ..." << endl;
			prepareBenchFile(PATH);
			compressWithWFLZ(filepaths, PATH);
			afterCompressing("wfLZ");
			break;
		case '3':
			cout << "Compressing files with QuickLZ..." << endl;
			prepareBenchFile(PATH);
			compressWithQuickLZ(filepaths, PATH);
			afterCompressing("QuickLZ");
			break;
		case '4':
			cout << "Compressing files with Snappy..." << endl;
			prepareBenchFile(PATH);
			compressWithSNAPPY(filepaths, PATH);
			afterCompressing("Snappy");
			break;
		case '5':
			cout << "Compressing files..." << endl;
			prepareBenchFile(PATH);
			compressWithLZ4(filepaths, PATH);
			compressWithWFLZ(filepaths, PATH);
			compressWithQuickLZ(filepaths, PATH);
			compressWithSNAPPY(filepaths, PATH);
			afterCompressing("LZ4, wfLZ, QuickLZ and Snappy");
			break;
		case 'e':
		case 'E':
			cout << "Exit" << endl;
			benchfile.close();
			exit(0);
			break;
		default:
			break;
		}

	}
}


