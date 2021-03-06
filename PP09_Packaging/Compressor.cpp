﻿#include <iostream>
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
uint64_t totalTime = 0;

// For storing the chosen path
char PATH[MAX_PATH];

// Names for bench file and compressed/decompressed files folders
string BENCH_FILE = "__results.csv";
string COMPRESSED_FOLDER = "__compressed_files";
string COMPRESSED_ZIP = "__compressed_files.zip";

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
		benchbuffer.push_back("Compression Time in \xB5s;");
		benchbuffer.push_back("Decompression Time in \xB5s;");
		benchbuffer.push_back("Original Size in KB;");
		benchbuffer.push_back("Compressed Size in KB;");
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
void addToBenchFile(string parentPath, bool isHeaderSet, string compMethod, string fileName, uint64_t compTime, 
	uint64_t decompTime, uint32_t origSize, uint32_t compSize) {

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
	benchbuffer.push_back(to_string(compTime) + ";");
	benchbuffer.push_back(to_string(decompTime) + ";");
	// Convert from byte to kilobyte
	benchbuffer.push_back(to_string((uint32_t) ceil((double) origSize / 1024)) + ";");
	benchbuffer.push_back(to_string((uint32_t) ceil((double) compSize / 1024)) + ";");

	benchbuffer.push_back(to_string((double) compSize / (double) origSize) + ";\n");

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
				// Ignore compressed folder
				string folder = path.substr(path.find_last_of("\\") + 1);
				if (folder != COMPRESSED_FOLDER) {
					SearchFolder(path, files);
				}

			}
			else {
				// Add filename to file-vector
				string file = path.substr(path.find_last_of("\\") + 1);
				// Ignore the bench and .zip files
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

// Creates the compressed file for the given parameters and returns the original file name
string createCompressedFile(string compFilePath, string parentPath, string extension, const char *compMem, uint32_t compSize) {

	ofstream compFile;
	string origFileName;
	// Get the file original file name from the file path
	const size_t last_slash_idx = compFilePath.rfind('\\');
	if (std::string::npos != last_slash_idx) {
		origFileName = compFilePath.substr(last_slash_idx + 1, compFilePath.length());
	}
	// Add the extension for the compressed file
	string compFileName = origFileName + "_compressed." + extension;
	compFile.open(parentPath + "\\" + COMPRESSED_FOLDER + "\\" + compFileName, std::ios::binary | std::ios::ate);
	compFile.write(compMem, compSize);
	compFile.close();
	return origFileName;
}

// Helper Malloc function
void* Malloc(const size_t size)
{
	return malloc(size);
	return malloc((size + 3)&(~0x3)); // assume sane allocators pad allocations to four bytes
}

// Compress the chosen files with LZ4 compression method and write the process details to the bench file
void compressWithLZ4(vector<string> filePaths, string parentPath, bool isFast) {

	// Variables necessary for the process
	uint32_t origSize, compSize;
	char *origMem, *compMem;
	const char* origFilePath;
	// Chunk size in bytes
	int chunkSize;
	// Optional Zip File - Initialization
	/*string zfstr = parentPath + "\\" + COMPRESSED_ZIP;
	const TCHAR* zipfile = zfstr.c_str();
	HZIP zipArchive = CreateZip(zipfile, 0, 0);*/

	// Whether the sub header for this process is set
	bool isHeaderSet = false;

	// Loop through the files to compress
	for (unsigned int i = 0; i < filePaths.size(); i++) {

		// Read the original file
		{
			origFilePath = filePaths[i].c_str();
			FILE* fh;
			errno_t err = fopen_s(&fh, origFilePath, "rb");
			if (fh == NULL)
			{
				cerr << "Error: Could not open file " << origFilePath << endl;
				continue;
			}
			fseek(fh, 0, SEEK_END);
			origSize = (uint32_t)ftell(fh);
			if (origSize == 0) {
				cerr << "Warning: Empty file " << origFilePath << endl;
				continue;
			}
			origMem = (char*)Malloc(origSize);
			if (origMem == NULL)
			{
				fclose(fh);
				cerr << "Error: Allocation failed.\n" << endl;
				continue;
			}
			fseek(fh, 0, SEEK_SET);
			if (fread(origMem, 1, origSize, fh) != origSize)
			{
				fclose(fh);
				cerr << "Error: File read failed.\n" << endl;
				continue;
			}
			fclose(fh);
		}

		// Setup timer
		LONGLONG g_Frequency, g_start, g_end;
		if (!QueryPerformanceFrequency((LARGE_INTEGER*)&g_Frequency))
			std::cerr << "Error: Performance Counter is unavailable" << std::endl;

		// Compression
		QueryPerformanceCounter((LARGE_INTEGER*)&g_start);

		// Change parameters depending on the speed choice
		int speed;
		if (isFast) {
			speed = 10;
			chunkSize = 4096;
		}
		else {
			speed = 1;
			chunkSize = 65536;
		}
		struct lz4chunkParameters *chunkParams = (struct lz4chunkParameters*)
			malloc(((origSize / (size_t)chunkSize) + 1) * sizeof(struct lz4chunkParameters));
		int numChunks = (int)((int)origSize / chunkSize) + 1;
		int maxCompChunkSize = LZ4_compressBound(chunkSize);
		int compBuffSize = numChunks * maxCompChunkSize;
		compMem = (char*)malloc((size_t)compBuffSize);

		// Init chunks data
		{
			size_t remaining = origSize;
			char* in = origMem;
			char* out = compMem;
			for (int i = 0; i < numChunks; i++)
			{
				chunkParams[i].id = i;
				chunkParams[i].origBuffer = in; 
				in += chunkSize;
				if ((int)remaining > chunkSize) { 
					chunkParams[i].origSize = chunkSize; 
					remaining -= chunkSize; 
				}
				else { 
					chunkParams[i].origSize = (int)remaining; 
					remaining = 0; 
				}
				chunkParams[i].compBuffer = out; 
				out += maxCompChunkSize;
				chunkParams[i].compSize = 0;
			}
		}

		// Warmimg up memory
		for (unsigned int i = 0; i < origSize; i++) compMem[i] = (char)i;

		// Compress chunk by chunk
		for (int chunk = 0; chunk < numChunks; chunk++) {
			chunkParams[chunk].compSize = LZ4_compress_fast(chunkParams[chunk].origBuffer, chunkParams[chunk].compBuffer,
				chunkParams[chunk].origSize, maxCompChunkSize, speed);
		}

		QueryPerformanceCounter((LARGE_INTEGER*)&g_end);
		double dTimeDiff = (((double)(g_end - g_start)) / ((double)g_Frequency));
		// End compression

		// Compression Time
		uint64_t compTime = dTimeDiff * 1000000;
		totalTime += compTime;

		// Create the compressed file
		compSize = 0;
		for (int i = 0; i < numChunks; i++) {
			compSize += chunkParams[i].compSize;
		}
		string origFileName = createCompressedFile(filePaths[i], parentPath, "lz4", compMem, compSize);

		 //Optional Zip File - Add file
		//ZipAdd(zipArchive, origFileName.c_str(), (TCHAR*)compMem, compSize);

		// Decompression
		QueryPerformanceCounter((LARGE_INTEGER*)&g_start);

		for (int chunk = 0; chunk < numChunks; chunk++) {
			LZ4_decompress_fast(chunkParams[chunk].compBuffer, chunkParams[chunk].origBuffer, chunkParams[chunk].origSize);
		}
		
		QueryPerformanceCounter((LARGE_INTEGER*)&g_end);
		dTimeDiff = (((double)(g_end - g_start)) / ((double)g_Frequency));
		// End decompression

		// Decompression time in microseconds
		uint64_t decompTime = dTimeDiff * 1000000;
		totalTime += decompTime;

		// Add the details to the bench file
		addToBenchFile(parentPath, isHeaderSet, "LZ4", origFileName, compTime, decompTime, origSize, compSize);
		isHeaderSet = true;

		// Clean up
		free(compMem);
		free(origMem);
	}

	// Optional Zip File - Close Archive
	//CloseZip(zipArchive);
}

// Source: https://github.com/ShaneYCG/wflz/blob/master/example/main.c
// with modifications
// Compress the chosen files with wfLZ compression method and write the process details to the bench file
void compressWithWFLZ(vector<string> filePaths, string parentPath, bool isFast) {

	// Variables necessary for the process
	uint32_t origSize, compSize;
	uint8_t *origMem, *compMem, *workMem;
	const char* origFilePath;

	// Whether the sub header for this process is set
	bool isHeaderSet = false;

	// Loop through the files to compress
	for (unsigned int i = 0; i < filePaths.size(); i++) {

		// Read the original file
		{
			origFilePath = filePaths[i].c_str();
			FILE* fh;
			errno_t err = fopen_s(&fh, origFilePath, "rb");
			if (fh == NULL)
			{
				cerr << "Error: Could not open file " << origFilePath << endl;
				continue;
			}
			fseek(fh, 0, SEEK_END);
			origSize = (uint32_t)ftell(fh);
			if (origSize == 0) {
				cerr << "Warning: Empty file " << origFilePath << endl;
				continue;
			}
			origMem = (uint8_t*)Malloc(origSize);
			if (origMem == NULL)
			{
				fclose(fh);
				cerr << "Error: Allocation failed.\n" << endl;
				continue;
			}
			fseek(fh, 0, SEEK_SET);
			if (fread(origMem, 1, origSize, fh) != origSize)
			{
				fclose(fh);
				cerr << "Error: File read failed.\n" << endl;
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

		// Setup timer
		LONGLONG g_Frequency, g_start, g_end;
		if (!QueryPerformanceFrequency((LARGE_INTEGER*)&g_Frequency))
			std::cerr << "Error: Performance Counter is unavailable" << std::endl;

		// Compression
		QueryPerformanceCounter((LARGE_INTEGER*)&g_start);

		compMem = (uint8_t*)Malloc(wfLZ_GetMaxCompressedSize(origSize));
		if (isFast) {
			compSize = wfLZ_CompressFast(origMem, origSize, compMem, workMem, 0);
		}
		else {
			compSize = wfLZ_Compress(origMem, origSize, compMem, workMem, 0);
		}

		QueryPerformanceCounter((LARGE_INTEGER*)&g_end);
		double dTimeDiff = (((double)(g_end - g_start)) / ((double)g_Frequency));
		// End compression

		// Compression Time
		uint64_t compTime = dTimeDiff * 1000000;
		totalTime += compTime;

		// Create the compressed file
		string origFileName = createCompressedFile(filePaths[i], parentPath, "wflz", (char*)compMem, compSize);

		// Decompression
		QueryPerformanceCounter((LARGE_INTEGER*)&g_start);

		wfLZ_Decompress(compMem, origMem);

		QueryPerformanceCounter((LARGE_INTEGER*)&g_end);
		dTimeDiff = (((double)(g_end - g_start)) / ((double)g_Frequency));
		// End decompression

		// Decompression time in microseconds
		uint64_t decompTime = dTimeDiff * 1000000;
		totalTime += decompTime;

		// Add the details to the bench file
		addToBenchFile(parentPath, isHeaderSet, "wfLZ", origFileName, compTime, decompTime, origSize, compSize);
		isHeaderSet = true;

		// Clean up
		free(compMem);
		free(origMem);
	}
}

// Compress the chosen files with QuickLZ compression method and write the process details to the bench file
// (Replacement of Pithy due to errors in code and lack of documentation)
void compressWithQuickLZ(vector<string> filePaths, string parentPath, bool isFast) {

	// Variables necessary for the process
	uint32_t origSize, compSize;
	char *origMem, *compMem;
	const char* origFilePath;

	// Whether the sub header for this process is set
	bool isHeaderSet = false;

	// Loop through the files to compress
	for (unsigned int i = 0; i < filePaths.size(); i++) {

		// Read the original file
		{
			origFilePath = filePaths[i].c_str();
			FILE* fh;
			errno_t err = fopen_s(&fh, origFilePath, "rb");
			if (fh == NULL)
			{
				cerr << "Error: Could not open file " << origFilePath << endl;
				continue;
			}
			fseek(fh, 0, SEEK_END);
			origSize = (uint32_t)ftell(fh);
			if (origSize == 0) {
				cerr << "Warning: Empty file " << origFilePath << endl;
				continue;
			}
			origMem = (char*)Malloc(origSize);
			if (origMem == NULL)
			{
				fclose(fh);
				cerr << "Error: Allocation failed.\n" << endl;
				continue;
			}
			fseek(fh, 0, SEEK_SET);
			if (fread(origMem, 1, origSize, fh) != origSize)
			{
				fclose(fh);
				cerr << "Error: File read failed.\n" << endl;
				continue;
			}
			fclose(fh);
		}

		// Setup timer
		LONGLONG g_Frequency, g_start, g_end;
		if (!QueryPerformanceFrequency((LARGE_INTEGER*)&g_Frequency))
			std::cerr << "Error: Performance Counter is unavailable" << std::endl;

		// Compression
		QueryPerformanceCounter((LARGE_INTEGER*)&g_start);

		// Change the compression level if fast is not wanted.
		// Default is set already set to the fastest level
		if (!isFast) {
			#ifdef QLZ_COMPRESSION_LEVEL
			#undef QLZ_COMPRESSION_LEVEL
			#endif
			#define QLZ_COMPRESSION_LEVEL 3
		}
		qlz_state_compress* stateCompress = (qlz_state_compress *)malloc(sizeof(qlz_state_compress));
		compMem = (char*)Malloc(origSize + 400);
		compSize = qlz_compress(origMem, compMem, origSize, stateCompress);

		QueryPerformanceCounter((LARGE_INTEGER*)&g_end);
		double dTimeDiff = (((double)(g_end - g_start)) / ((double)g_Frequency));
		// End compression

		// Compression Time
		uint64_t compTime = dTimeDiff * 1000000;
		totalTime += compTime;

		// Create the compressed file
		string origFileName = createCompressedFile(filePaths[i], parentPath, "qlz", compMem, compSize);

		// Decompression
		QueryPerformanceCounter((LARGE_INTEGER*)&g_start);

		qlz_state_decompress *stateDecompress = (qlz_state_decompress *)malloc(sizeof(qlz_state_decompress));
		unsigned int decompSize = qlz_size_decompressed(compMem);
		char* decompMem = (char*)malloc(decompSize);
		decompSize = qlz_decompress(compMem, decompMem, stateDecompress);

		QueryPerformanceCounter((LARGE_INTEGER*)&g_end);
		dTimeDiff = (((double)(g_end - g_start)) / ((double)g_Frequency));
		// End decompression

		// Decompression time in microseconds
		uint64_t decompTime = dTimeDiff * 1000000;
		totalTime += decompTime;

		// Add the details to the bench file
		addToBenchFile(parentPath, isHeaderSet, "QuickLZ", origFileName, compTime, decompTime, origSize, compSize);
		isHeaderSet = true;

		// Clean up
		free(stateCompress);
		free(stateDecompress);
		free(compMem);
		free(origMem);
		free(decompMem);
	}
}

// source: https://snappy.angeloflogic.com/downloads/
// with modifications
// Compress the chosen files with Snappy compression method and write the process details to the bench file
void compressWithSNAPPY(vector<string> filePaths, string parentPath) {

	// Variables necessary for the process
	uint32_t origSize, compSize, decompSize;
	char *origMem, *compMem;
	const char* origFilePath;

	// Whether the sub header for this process is set
	bool isHeaderSet = false;

	// Optional Zip File - Initialization - copy all ZIP-related code to same place in other compressWithXXX()-function for use of other algorithm
	string zfstr = parentPath + "\\" + COMPRESSED_ZIP;
	const TCHAR* zipfile = zfstr.c_str();
	HZIP zipArchive = CreateZip(zipfile, 0, 0);

	// Loop through the files to compress
	for (unsigned int i = 0; i < filePaths.size(); i++) {

		// Read the original file
		{
			origFilePath = filePaths[i].c_str();
			FILE* fh;
			errno_t err = fopen_s(&fh, origFilePath, "rb");
			if (fh == NULL)
			{
				cerr << "Error: Could not open file " << origFilePath << endl;
				continue;
			}
			fseek(fh, 0, SEEK_END);
			origSize = (uint32_t)ftell(fh);
			if (origSize == 0) {
				cerr << "Warning: Empty file " << origFilePath << endl;
				continue;
			}
			origMem = (char*)Malloc(origSize);
			if (origMem == NULL)
			{
				fclose(fh);
				cerr << "Error: Allocation failed.\n" << endl;
				continue;
			}
			fseek(fh, 0, SEEK_SET);
			if (fread(origMem, 1, origSize, fh) != origSize)
			{
				fclose(fh);
				cerr << "Error: File read failed.\n" << endl;
				continue;
			}
			fclose(fh);
		}

		// Setup timer
		LONGLONG g_Frequency, g_start, g_end;
		if (!QueryPerformanceFrequency((LARGE_INTEGER*)&g_Frequency))
			std::cerr << "Error: Performance Counter is unavailable" << std::endl;

		// Compression
		QueryPerformanceCounter((LARGE_INTEGER*)&g_start);

		compSize = snappy_max_compressed_length(origSize);
		compMem = new char[compSize];
		snappy_status comp_status = snappy_compress(origMem, origSize, compMem, &compSize);

		QueryPerformanceCounter((LARGE_INTEGER*)&g_end);
		double dTimeDiff = (((double)(g_end - g_start)) / ((double)g_Frequency));
		// End compression

		// Compression Time
		uint64_t compTime = dTimeDiff * 1000000;
		totalTime += compTime;

		// Create the compressed file
		string origFileName = createCompressedFile(filePaths[i], parentPath, "snappy", compMem, compSize);

		//Optional Zip File - Add file
		ZipAdd(zipArchive, origFileName.c_str(), (TCHAR*)compMem, compSize);

		// Decompression
		QueryPerformanceCounter((LARGE_INTEGER*)&g_start);

		snappy_status size_status = snappy_uncompressed_length(compMem, compSize, &decompSize);
		char* decompMem = (char*)malloc(decompSize);
		snappy_status status = snappy_uncompress(compMem, compSize, decompMem, &decompSize);

		QueryPerformanceCounter((LARGE_INTEGER*)&g_end);
		dTimeDiff = (((double)(g_end - g_start)) / ((double)g_Frequency));
		// End decompression

		// Decompression time in microseconds
		uint64_t decompTime = dTimeDiff * 1000000;
		totalTime += decompTime;
		
		// Add the details to the bench file
		addToBenchFile(parentPath, isHeaderSet, "Snappy", origFileName, compTime, decompTime, origSize, compSize);
		isHeaderSet = true;

		// Clean up
		free(compMem);
		free(decompMem);
		free(origMem);
	}
	// Optional Zip File - Close Archive
	CloseZip(zipArchive);
}

// Do after every compression process
void afterCompressing(string compressionMethod) {

	// Message to show right after the compression
	cout << "Compressing with " << compressionMethod << " complete! That took " << totalTime << " microseconds." << endl
		<< endl << "See the " << BENCH_FILE << " file for details." << endl;
	cout << "Do you want to open the directory? (y / n)" << endl;

	// Reset the total time counter
	totalTime = 0;

	// Yes/No dialog to show the chosen folder
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

	// Show the algorithm choice dialog again
	cout << endl << "Please select algorithm: " << endl << "Press key 1 for LZ4" << endl <<
		"Press key 2 for wfLZ" << endl << "Press key 3 for QuickLZ" << endl << "Press key 4 for Snappy" << endl <<
		"Press key 5 for all of the above" << endl << "Press key e key to exit program" << endl;
}


int main(void) {

	// Paths of the files to compress
	vector<string> filePaths;
	bool isFast;

	// User chooses the folder whose files and subfolders to be compressed
	cout << "Please select folder to compress" << endl;
	filePaths = selectFolder(PATH);

	cout << "Folder selected: " << PATH << endl << endl;

	// User chooses to use the fast or normal versions of the algorithms
	cout << "Do you want to use the fast versions of the algorithms? (y / n)" << endl <<
		"Note: This has no effect on Snappy." << endl <<
		"Warning: Non-fast version of wfLZ is extremely slow." << endl;
	bool speedChosen = false;
	while (!speedChosen) {
		switch (getchar()) {
		case 'Y':
		case 'y':
			isFast = true;
			break;
		case 'N':
		case 'n':
			isFast = false;
			break;
		default:
			isFast = true;
		}
		speedChosen = true;
	}

	// Show algorith choice dialog
	cout << endl << "Please select algorithm: " << endl << "Press key 1 for LZ4" << endl <<
		"Press key 2 for wfLZ" << endl << "Press key 3 for QuickLZ" << endl << "Press key 4 for Snappy" << endl <<
		"Press key 5 for all of the above" << endl << "Press key e key to exit program" << endl;

	// This makes the user only able to open the file in read-only mode while the program is still running
	// Otherwise benchmark doesn't write while the file is open
	string benchName = (string)PATH + "\\" + BENCH_FILE;
	remove(benchName.c_str());
	ofstream benchfile;
	benchfile.open(benchName, std::ios_base::app);

	// Create output directories
	if (!(CreateDirectory(string((string)PATH + "\\" + COMPRESSED_FOLDER + "\\").c_str(), NULL) ||
		ERROR_ALREADY_EXISTS == GetLastError())) {
		cerr << "Error: Failed to create output directories." << endl;
		return -1;
	}

	// User's choice
	char choice = 0;
	while (true) {
		choice = getchar();
		cout << endl;

		switch (choice) {
		case '1':
			cout << "Compressing files with LZ4..." << endl;
			prepareBenchFile(PATH);
			compressWithLZ4(filePaths, PATH, isFast);
			afterCompressing("LZ4");
			break;
		case '2':
			cout << "Compressing files with wfLZ..." << endl;
			prepareBenchFile(PATH);
			compressWithWFLZ(filePaths, PATH, isFast);
			afterCompressing("wfLZ");
			break;
		case '3':
			cout << "Compressing files with QuickLZ..." << endl;
			prepareBenchFile(PATH);
			compressWithQuickLZ(filePaths, PATH, isFast);
			afterCompressing("QuickLZ");
			break;
		case '4':
			cout << "Compressing files with Snappy..." << endl;
			prepareBenchFile(PATH);
			compressWithSNAPPY(filePaths, PATH);
			afterCompressing("Snappy");
			break;
		case '5':
			cout << "Compressing files..." << endl;
			prepareBenchFile(PATH);
			compressWithLZ4(filePaths, PATH, isFast);
			compressWithWFLZ(filePaths, PATH, isFast);
			compressWithQuickLZ(filePaths, PATH, isFast);
			compressWithSNAPPY(filePaths, PATH);
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


