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

#include "lz4.h"
#include "wfLZ.h"
#include "pithy.h"

using namespace std;

#ifndef NULL
#define NULL 0
#endif

#define _SECOND ((int64) 10000000)
#define _MINUTE (60 * _SECOND)
#define _HOUR   (60 * _MINUTE)
#define _DAY    (24 * _HOUR)

boolean isBenchFilePrepared = false;

struct lz4chunkParameters
{
	int   id;
	char* origBuffer;
	char* compressedBuffer;
	int   origSize;
	int   compressedSize;
};

void prepareBenchFile(string parentPath) {
	if (!isBenchFilePrepared) {
		ofstream benchfile;
		benchfile.open(parentPath + "\\" + "results.csv", std::ios_base::app);
		vector<string> benchbuffer(1);
		benchbuffer.push_back("Compressed File Name;");
		benchbuffer.push_back("Decompression Time;");
		benchbuffer.push_back("Compression Ratio;\n");
		for (int i = 0; i < benchbuffer.size(); i++) {
			benchfile << benchbuffer[i];
		}
		benchbuffer.clear();
		isBenchFilePrepared = true;
	}
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
			//Append the file found on to the path of the 
			//directory the user chose
			path.append("\\");
			path.append(FindFileData.cFileName);
			
			//If SetCurrentDirectory Succeeds ( returns 1 ) the 
			//current file is a directory. Pause this function,             
			//and have it call itself. This will begin the whole 
			//process over in a sub directory.             
			if ((SetCurrentDirectory(path.data())))
			{
				SearchFolder(path, files);
			} else {
				// add filename to file-vector
				files->push_back(path);
			}
		}
	} while (FindNextFile(hFind, &FindFileData)
		&& hFind != INVALID_HANDLE_VALUE);
	FindClose(hFind);
}//SEARCH FOLDER

// selectFolder()
vector<string> selectFolder(char *path) {
	vector<string> filepaths;
	
	BROWSEINFO bi = { 0 };
	bi.lpszTitle = "Select folder for compression.";
	LPITEMIDLIST pidl = SHBrowseForFolder(&bi);

	if (pidl != 0) {
		SHGetPathFromIDList(pidl, path);

		SetCurrentDirectory(path);

		SearchFolder(path, &filepaths);

		IMalloc * imalloc = 0;
		if (SUCCEEDED(SHGetMalloc(&imalloc))) {
			imalloc->Free(pidl);
			imalloc->Release();
		}
	}

	return filepaths;
} // selectFolder()

void compressWithLZ4(vector<string> filepaths, string parentpath) {
	
	// Header for bench file
	vector<string> benchbuffer(1);
	benchbuffer.push_back("--- lz4 ---;---;---;\n");

	for (unsigned int i = 0; i < filepaths.size(); i++) {
	
		FILE* uncompFile = NULL;
		FILE* compFile = NULL;
		FILE* decompFile = NULL;
		const int chunkSize = 65536;

		const char* uncompFileName = filepaths[i].c_str();
		string filename;
		const size_t last_slash_idx = filepaths[i].rfind('\\');
		if (std::string::npos != last_slash_idx) {
			filename = filepaths[i].substr(last_slash_idx + 1, filepaths[i].length());
		}
		string uncompressedFileName = filename;
		filename.append("_compressed.lz4");
		string compStr = parentpath + "\\" + filename;
		const char* compFileName = compStr.c_str();

		string decompStr = parentpath + "\\" + uncompressedFileName;
		const char* decompFileName = decompStr.c_str();
		
		char* inbuffer;
		char* outbuffer;
		int num_read = 0;
		unsigned long total_read = 0, total_wrote = 0;
		int numberOfChunks = 0;

		fopen_s(&uncompFile, uncompFileName, "rb");
		fopen_s(&compFile, compFileName, "wb");
		
		if (!compFile || !uncompFile) {
			printf("Couldn't open file!\n");
			continue;
		}
		struct stat buf;
		if (stat(uncompFileName, &buf) == 0) {
			//	printf("Groesse der Datei: %i\n", buf.st_size);
		}
		int fileSize = buf.st_size;
		/* Alloc */
		struct lz4chunkParameters* chunkP;
		chunkP = (struct lz4chunkParameters*) malloc(((fileSize / (size_t)chunkSize) + 1) * sizeof(struct lz4chunkParameters));
		inbuffer = (char*)malloc((size_t)fileSize);
		numberOfChunks = (int)((int)fileSize / chunkSize) + 1;
		int maxCompressedChunkSize = LZ4_compressBound(chunkSize);
		int compressedBuffSize = numberOfChunks * maxCompressedChunkSize;
		outbuffer = (char*)malloc((size_t)compressedBuffSize);



		/* Init chunks data */
		{
			int i;
			size_t remaining = fileSize;
			char* in = inbuffer;
			char* out = outbuffer;
			for (i = 0; i < numberOfChunks; i++)
			{
				chunkP[i].id = i;
				chunkP[i].origBuffer = in; in += chunkSize;
				if ((int)remaining > chunkSize) { chunkP[i].origSize = chunkSize; remaining -= chunkSize; }
				else { chunkP[i].origSize = (int)remaining; remaining = 0; }
				chunkP[i].compressedBuffer = out; out += maxCompressedChunkSize;
				chunkP[i].compressedSize = 0;
			}
		}

		/* Fill input buffer */
		printf("Loading %s...       \r", uncompFileName);
		int readSize = fread(inbuffer, 1, fileSize, uncompFile);
		fclose(uncompFile);

		if (readSize != fileSize)
		{
			printf("\nError: problem reading file '%s' !!    \n", uncompFileName);
			free(inbuffer);
			free(outbuffer);
			free(chunkP);
			continue;
		}

		// Compression
		for (int i = 0; i < fileSize; i++) outbuffer[i] = (char)i;     /* warmimg up memory */

		for (int chunk = 0; chunk < numberOfChunks; chunk++) {

			chunkP[chunk].compressedSize = LZ4_compress_fast(chunkP[chunk].origBuffer, chunkP[chunk].compressedBuffer, chunkP[chunk].origSize, maxCompressedChunkSize, 2);

		}
		int compressedFileSize = 0;
		for (int i = 0; i < numberOfChunks; i++) {
			compressedFileSize += chunkP[i].compressedSize;
		}
		char* out = outbuffer;
		fwrite(out, 1, compressedFileSize, compFile);
		fclose(compFile);
		//printf("Compression finished!\n");

		////////////////////////////////////////////////////////////////////////////////////

		/* Swap buffers */
		char* tmp = inbuffer;
		inbuffer = outbuffer;
		outbuffer = tmp;
		fopen_s(&compFile, compFileName, "rb");
		printf("Loading %s...       \n", compFileName);
		readSize = fread(inbuffer, 1, compressedFileSize, compFile);
		fclose(compFile);

		if (readSize != compressedFileSize)
		{
			printf("\nError: problem reading file '%s' !!    \n", uncompFileName);
			free(inbuffer);
			free(outbuffer);
			free(chunkP);
			continue;
		}

		// Decompression
		LPSYSTEMTIME start = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));
		LPSYSTEMTIME end = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));

		GetSystemTime(start);
		for (int chunk = 0; chunk < numberOfChunks; chunk++) {

			LZ4_decompress_fast(chunkP[chunk].compressedBuffer, chunkP[chunk].origBuffer, chunkP[chunk].origSize);

		}
		GetSystemTime(end);

		int elapsedTime = 1000 * (end->wSecond - start->wSecond) + end->wMilliseconds - start->wMilliseconds;

		// Write decompressed files
		//fopen_s(&decompFile, decompFileName, "wb");
		//if (!decompFile) {
		//	printf("Couldn't open file %s!\n", decompFileName);
		//	continue;
		//}
		//fwrite(outbuffer, 1, fileSize, decompFile);
		//fclose(decompFile);

		//printf("Decompression finished!\n");
		// End of decompression

		cout << "Decompression time: " << elapsedTime << " seconds" << endl;

		ofstream benchfile;
		benchfile.open(parentpath + "\\" + "results.csv", std::ios_base::app);

		// Write benchmark data to csv-file
		benchbuffer.push_back(filename);
		benchbuffer.push_back(";");
		benchbuffer.push_back(to_string(elapsedTime) + " s");
		benchbuffer.push_back(";");
		benchbuffer.push_back(to_string((double)compressedFileSize / (double)fileSize));
		benchbuffer.push_back(";");
		benchbuffer.push_back("\n");

		for (int i = 0; i < benchbuffer.size(); i++) {
			benchfile << benchbuffer[i];
		}
		// end of benchmark

		// cleanup
		free(inbuffer);
		free(outbuffer);
		benchbuffer.clear();

	}
	cout << "Lz4 complete!" << endl;
}

void* Malloc(const size_t size)
{
	return malloc(size);
	return malloc((size + 3)&(~0x3)); // assume sane allocators pad allocations to four bytes
}

// Source: https://github.com/ShaneYCG/wflz/blob/master/example/main.c
// with modifications
void compressWithWFLZ(vector<string> filepaths, string parentpath) {
	const char* uncompFileName;

	uint32_t compressedSize, uncompressedSize;
	uint8_t* uncompressed;
	uint8_t* workMem;
	uint8_t* compressed;
	errno_t err;

	// Header for bench file
	vector<string> benchbuffer(1);
	benchbuffer.push_back("--- wfLZ ---;---;---;\n");

	for (unsigned int i = 0; i < filepaths.size(); i++) {
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
			cout << "Uncompressed size of file " << i << ": " << uncompressedSize << endl;
			uncompressed = (uint8_t*)Malloc(uncompressedSize);
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
		workMem = (uint8_t*)Malloc(wfLZ_GetWorkMemSize());
		if (workMem == NULL)
		{
			cout << "Error: Allocation failed.\n" << endl;
			continue;
		}

		compressed = (uint8_t*)Malloc(wfLZ_GetMaxCompressedSize(uncompressedSize));
		compressedSize = wfLZ_CompressFast(uncompressed, uncompressedSize, compressed, workMem, 0);

		ofstream outputfile;
		string outputfilename;
		outputfilename = filepaths[i];

		outputfilename.substr();

		string compressedFileName;
		const size_t last_slash_idx = outputfilename.rfind('\\');
		if (std::string::npos != last_slash_idx) {
			compressedFileName = outputfilename.substr(last_slash_idx + 1, outputfilename.length());
		}

		compressedFileName.append("_compressed.wfLZ");

		outputfile.open(parentpath + "\\" + compressedFileName, std::ios::binary | std::ios::ate);
		const char* temp = (char*)compressed;
		outputfile.write(temp, compressedSize);

		cout << "file " << compressedFileName << " compressed." << endl;
		cout << "Compressed size of file " << i << ": " << compressedSize << endl;

		outputfile.close();
		// Decompression
		LPSYSTEMTIME start = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));
		LPSYSTEMTIME end = (LPSYSTEMTIME)malloc(2 * sizeof(LPSYSTEMTIME));

		GetSystemTime(start);
		wfLZ_Decompress(compressed, uncompressed);
		GetSystemTime(end);

		int elapsedTime = 1000 * (end->wSecond - start->wSecond) + end->wMilliseconds - start->wMilliseconds;

		ofstream benchfile;
		benchfile.open(parentpath + "\\" + "results.csv", std::ios_base::app);

		// Write benchmark data to csv-file
		benchbuffer.push_back(compressedFileName + ";");
		benchbuffer.push_back(to_string(elapsedTime) + " s;");
		benchbuffer.push_back(to_string((float)compressedSize / (float)uncompressedSize) + ";\n");

		for (int i = 0; i < benchbuffer.size(); i++) {
			benchfile << benchbuffer[i];
		}

		free(workMem);
		free(compressed);

		free(uncompressed);
		benchbuffer.clear();

		printf("Compression Ratio: %.2f\n\n", ((float)compressedSize) / ((float)uncompressedSize));
	}
	// TO DO: write file into zip folder,
	cout << "Compression complete!" << endl;
}

void compressWithPITHY(vector<string> filepaths, string parentpath) {
	// TO DO
}

void compressWithSNAPPY(vector<string> filepaths, string parentpath) {
	// TO DO
}

int main(void) {

	cout << "Please select folder to compress" << endl;
	char path[MAX_PATH];
	vector<string> filepaths;
	filepaths = selectFolder(path);

	cout << "Folder selected: " << path << endl << endl;

	cout << "Please select algorithm: " << endl << "Press key 1 for lz4" << endl <<
		"Press key 2 for wfLZ" << endl << "Press key 3 for pithy" << endl << "Press key 4 for snappy" << endl <<
		"Press key 5 for all of the above" << endl << "Press key e key to exit program" << endl;

	// This makes the user only able to open the file in read-only mode while the program is still running
	// Otherwise benchmark doesn't write while the file is open
	ofstream benchfile;
	benchfile.open((string) path + "\\" + "results.csv", std::ios_base::app);

	char mode = 0;

	while (true) {

		mode = getchar();

		cout << endl;
		switch (mode) {
		case '1':
			cout << "Compressing files with lz4..." << endl;
			prepareBenchFile(path);
			compressWithLZ4(filepaths, path);
			cout << "Compressing with lz4 done!" << endl 
				<< "See the results.csv file." << endl;
			break;
		case '2':
			cout << "Compressing files with wflz..." << endl;
			prepareBenchFile(path);
			compressWithWFLZ(filepaths, path);
			cout << "Compressing with wfLZ done!" << endl
				<< "See the results.csv file." << endl;
			break;
		case '3':
			cout << "Compressing files with pithy..." << endl;
			prepareBenchFile(path);
			compressWithPITHY(filepaths, path);
			cout << "Compressing with pithy done!" << endl;
			break;
		case '4':
			cout << "Compressing files with snappy..." << endl;
			prepareBenchFile(path);
			compressWithSNAPPY(filepaths, path);
			cout << "Compressing with snappy done!" << endl;
			break;
		case '5':
			cout << "Compressing files..." << endl;
			prepareBenchFile(path);
			compressWithLZ4(filepaths, path);
			compressWithWFLZ(filepaths, path);
			compressWithPITHY(filepaths, path);
			compressWithSNAPPY(filepaths, path);
			cout << "Compressing done!" << endl;
			break;
		case 'e':
		case 'E':
			cout << "exit" << endl;
			exit(0);
			break;
		default:
			break;
		}

	}
}


