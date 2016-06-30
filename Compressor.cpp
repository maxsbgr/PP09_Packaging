#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <ShlObj.h>
#include <windows.h>
#include <string>
#include <vector>
#include <iterator>
#include "Shlwapi.h"

#include "lz4.h"

using namespace std;

struct lz4chunkParameters
{
	int   id;
	char* origBuffer;
	char* compressedBuffer;
	int   origSize;
	int   compressedSize;
};

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
	LPITEMIDLIST pidl = SHBrowseForFolder( &bi );

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
	
	for (int i = 0; i < filepaths.size(); i++) {
		ifstream file;
		file.open(filepaths[i]);
		file.seekg(0, file.end);
		streamsize size = file.tellg();
		file.seekg (0, file.beg);
    	cout << "Size of file " << i << ": " << size << endl;
		vector<char> inbuffer;
		inbuffer.reserve(size);
		inbuffer.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());

		if (!inbuffer.empty())	{
			// file successfully read
			const int chunkSize = 32768;
			
			int num_read = 0;
			unsigned long total_read = 0, total_wrote = 0;
			int numberOfChunks = 0;

			/* Alloc */
			struct lz4chunkParameters* chunkP = (struct lz4chunkParameters*) malloc(((size / (size_t)chunkSize)+1) * sizeof(struct lz4chunkParameters));
			numberOfChunks = (int) ((int)size / chunkSize) + 1;
			int maxCompressedChunkSize = LZ4_compressBound(chunkSize);
			int compressedBuffSize = numberOfChunks * maxCompressedChunkSize;
			
			cout << "number of chunks " << i << ": " << numberOfChunks << endl;

			vector<char> outbuffer(compressedBuffSize);

			/* Init chunks data */
			{
				int i;
				size_t remaining = size;
				char* in = &inbuffer[0];
				char* out = &outbuffer[0];

				for (i=0; i<numberOfChunks; i++)
				{
					chunkP[i].id = i;
					chunkP[i].origBuffer = in; in += chunkSize;
					if ((int)remaining > chunkSize) { chunkP[i].origSize = chunkSize; remaining -= chunkSize; } else { chunkP[i].origSize = (int)remaining; remaining = 0; }
					chunkP[i].compressedBuffer = out; out += maxCompressedChunkSize;
					chunkP[i].compressedSize = 0;
				}
			}

			// Compression
			for (int i=0; i < size; i++) outbuffer[i]=(char)i;     /* warmimg up memory */

			for (int chunk=0; chunk<numberOfChunks; chunk++) {

				chunkP[chunk].compressedSize = LZ4_compress_fast(chunkP[chunk].origBuffer, chunkP[chunk].compressedBuffer, chunkP[chunk].origSize, maxCompressedChunkSize, 2);

			}
			int compressedFileSize = 0;
			for (int i=0; i<numberOfChunks; i++) {
				compressedFileSize += chunkP[i].compressedSize;
			}

			ofstream outputfile;
			string outputfilename;
			outputfilename = filepaths[i];

			outputfilename.substr();

			//PathRemoveFileSpec(outputfilename);

			
			string filename;
			const size_t last_slash_idx = outputfilename.rfind('\\');
			if (std::string::npos != last_slash_idx) {
			    filename = outputfilename.substr(last_slash_idx+1, outputfilename.length());
			}

			filename.append("_compressed.lz4");

			outputfile.open(parentpath+"\\"+filename, std::ios::binary | std::ios::ate);
			outputfile.write(outbuffer.data(), compressedFileSize);

			cout << "file " << filename << " compressed." << endl;

			outputfile.close();
			file.close();

			// cleanup
			inbuffer.clear();
			outbuffer.clear();


		} else {
			cout << "Could not open file " << filepaths[i] << endl;
			file.close();
		}
	}
	// TO DO: write file into zip folder, add decompression info
	cout << "Compression complete!" << endl;

}

void compressWithWFLZ(vector<string> filepaths, string parentpath) {
	// TO DO
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

	cout << "Folder sekected: " << path << endl << endl;

	cout << "Please select algorithm: " << endl << "Press key 1 for lz4" << endl <<
		"Press key 2 for wflz" << endl << "Press key 3 for pithy" << endl << "Press key 4 for snappy" << endl <<
		"Press key 5 for all of the above" << endl << "Press key e key to exit program" << endl ;

	char mode = 0;
	
	while(true) {

		mode = getchar();

		switch (mode) {
		case '1':
			cout << "Compressing files with lz4..." << endl;
			compressWithLZ4(filepaths, path);
			cout << "Compressing with lz4 done!" << endl;
			break;
		case '2':
			cout << "Compressing files with wflz..." << endl;
			compressWithWFLZ(filepaths, path);
			cout << "Compressing with wflz done!" << endl;
			break;
		case '3':
			cout << "Compressing files with pithy..." << endl;
			compressWithPITHY(filepaths, path);
			cout << "Compressing with pithy done!" << endl;
			break;
		case '4':
			cout << "Compressing files with snappy..." << endl;
			compressWithSNAPPY(filepaths, path);
			cout << "Compressing with snappy done!" << endl;
			break;
		case '5':
			cout << "Compressing files..." << endl;
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


