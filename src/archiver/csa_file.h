#ifndef _CSA_FILE_H_
#define _CSA_FILE_H_
#include <csa_common.h>
// File types accepting UTF-8 filenames

#ifdef _MSC_VER  // Microsoft C++
#define fseeko(a,b,c) _fseeki64(a,b,c)
#define ftello(a) _ftelli64(a)
#else
#ifndef unix
#ifndef fseeko
#define fseeko(a,b,c) fseeko64(a,b,c)
#endif
#ifndef ftello
#define ftello(a) ftello64(a)
#endif
#endif
#endif

#ifdef unix

class InputFile {
  FILE* in;

public:
  InputFile(): in(0) {}

  bool open(const char* filename) {
    in=fopen(filename, "rb");
    if (!in) perror(filename);
    return in!=0;
  }

  // True if open
  bool isopen() {return in!=0;}

  // Return file position
  int64_t tell() {
    return ftello(in);
  }

  uint32_t read(char *buf, uint32_t size) {
      return fread(buf, 1, size, in);
  }

  // Set file position
  void seek(int64_t pos, int whence) {
    if (whence==SEEK_CUR) {
      whence=SEEK_SET;
      pos+=tell();
    }
    fseeko(in, pos, whence);
  }

  // Close file if open
  void close() {if (in) fclose(in); in = 0;}
  ~InputFile() {close();}
};

class OutputFile {
  FILE* out;
  string filename;
  bool dummy;
public:
  OutputFile(): out(0) {}

  // Return true if file is open
  bool isopen() {return out!=0;}

  // Open for append/update or create if needed.
  // If aes then encrypt with aes+eoff.
  bool open(const char* filename) {
    assert(!isopen());
    this->filename=filename;
    dummy = (this->filename == DUMMY_FILENAME);
    if (dummy) {
        out = (FILE *)1;
        return true;
    }
    out=fopen(filename, "rb+");
    if (!out) out=fopen(filename, "wb+");
    if (!out) perror(filename);
    if (out) fseeko(out, 0, SEEK_END);
    return isopen();
  }

  // Write bufp[0..size-1]
  void write(const char* bufp, int size) {
      if (!dummy) fwrite(bufp, 1, size, out);
  }

  // Write size bytes at offset
  void write(const char* bufp, int64_t pos, int size) {
    assert(isopen());
    if (!dummy) {
        fseeko(out, pos, SEEK_SET);
        write(bufp, size);
    }
  }

  // Seek to pos. whence is SEEK_SET, SEEK_CUR, or SEEK_END
  void seek(int64_t pos, int whence) {
    assert(isopen());
    if (!dummy) {
        fseeko(out, pos, whence);
    }
  }

  // return position
  int64_t tell() {
    assert(isopen());
    if (!dummy)
        return ftello(out);
    else
        return 0;
  }

  // Truncate file and move file pointer to end
  void truncate(int64_t newsize=0) {
    assert(isopen());
    seek(newsize, SEEK_SET);
    if (!dummy && ftruncate(fileno(out), newsize)) perror("ftruncate");
  }

  // Close file and set date if not 0. Set permissions if attr low byte is 'u'
  void close(int64_t date=0, int64_t attr=0) {
    if (dummy) {
        dummy = false;
        out = 0;
        return;
    }
    if (out) {
      fclose(out);
    }
    out=0;
    if (date>0) {
      struct utimbuf ub;
      ub.actime=time(NULL);
      ub.modtime=unix_time(date);
      utime(filename.c_str(), &ub);
    }
    if ((attr&255)=='u')
      chmod(filename.c_str(), attr>>8);
  }

  ~OutputFile() {close();}
};

#else  // Windows

class InputFile {
  HANDLE in;  // input file handle
public:
  InputFile():
    in(INVALID_HANDLE_VALUE) {}

  // Open for reading. Return true if successful.
  // Encrypt with aes+e if aes.
  bool open(const char* filename) {
    assert(in==INVALID_HANDLE_VALUE);
    std::wstring w=utow(filename, true);
    in=CreateFile(w.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (in==INVALID_HANDLE_VALUE){
        fprintf(stderr, "File open error %s\n", filename);
    }
    return in!=INVALID_HANDLE_VALUE;
  }

  bool isopen() {return in!=INVALID_HANDLE_VALUE;}

  // set file pointer
  void seek(int64_t pos, int whence) {
    if (whence==SEEK_SET) whence=FILE_BEGIN;
    else if (whence==SEEK_END) whence=FILE_END;
    else if (whence==SEEK_CUR) {
      whence=FILE_BEGIN;
      pos+=tell();
    }
    LONG offhigh=pos>>32;
    SetFilePointer(in, pos, &offhigh, whence);
  }

  // get file pointer
  int64_t tell() {
    LONG offhigh=0;
    DWORD r=SetFilePointer(in, 0, &offhigh, FILE_CURRENT);
    return (int64_t(offhigh)<<32)+r+ptr-n;
  }

  // Close handle if open
  void close() {
    if (in!=INVALID_HANDLE_VALUE) {
      CloseHandle(in);
      in=INVALID_HANDLE_VALUE;
    }
  }
  ~InputFile() {close();}
};

class OutputFile {
  HANDLE out;               // output file handle
  std::wstring filename;    // filename as wide string
public:
  OutputFile(): out(INVALID_HANDLE_VALUE) {}

  // Return true if file is open
  bool isopen() {
    return out!=INVALID_HANDLE_VALUE;
  }

  // Open file ready to update or append, create if needed.
  // If aes then encrypt with aes+e.
  bool open(const char* filename_) {
    assert(!isopen());
    ptr=0;
    filename=utow(filename_, true);
    out=CreateFile(filename.c_str(), GENERIC_READ | GENERIC_WRITE,
                   0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out==INVALID_HANDLE_VALUE) {
        fprintf(stderr, "File open error %s\n", filename);
    } else {
      LONG hi=0;
      SetFilePointer(out, 0, &hi, FILE_END);
    }
    return isopen();
  }

  // Write pending output
  void flush() {
    assert(isopen());
    if (ptr) {
      DWORD n=0;
      if (aes) {
        int64_t off=tell()-ptr+eoff;
        if (off<32) error("attempt to overwrite salt");
        aes->encrypt(&buf[0], ptr, off);
      }
      WriteFile(out, &buf[0], ptr, &n, NULL);
      if (ptr!=int(n)) {
        fprintf(stderr, "%s: error %d: wrote %d of %d bytes\n",
                wtou(filename.c_str()).c_str(), int(GetLastError()),
                int(n), ptr);
        error("write failed");
      }
      ptr=0;
    }
  }

  // Write bufp[0..size-1]
  void write(const char* bufp, int size);

  // Write size bytes at offset
  void write(const char* bufp, int64_t pos, int size) {
    assert(isopen());
    flush();
    if (pos!=tell()) seek(pos, SEEK_SET);
    write(bufp, size);
  }

  // set file pointer
  void seek(int64_t pos, int whence) {
    if (whence==SEEK_SET) whence=FILE_BEGIN;
    else if (whence==SEEK_CUR) whence=FILE_CURRENT;
    else if (whence==SEEK_END) whence=FILE_END;
    flush();
    LONG offhigh=pos>>32;
    SetFilePointer(out, pos, &offhigh, whence);
  }

  // get file pointer
  int64_t tell() {
    LONG offhigh=0;
    DWORD r=SetFilePointer(out, 0, &offhigh, FILE_CURRENT);
    return (int64_t(offhigh)<<32)+r+ptr;
  }

  // Truncate file and move file pointer to end
  void truncate(int64_t newsize=0) {
    seek(newsize, SEEK_SET);
    SetEndOfFile(out);
  }

  // Close file and set date if not 0. Set attr if low byte is 'w'.
  void close(int64_t date=0, int64_t attr=0) {
    if (isopen()) {
      flush();
      setDate(out, date);
      CloseHandle(out);
      out=INVALID_HANDLE_VALUE;
      if ((attr&255)=='w')
        SetFileAttributes(filename.c_str(), attr>>8);
      filename=L"";
    }
  }
  ~OutputFile() {close();}
};

#endif

/*
string output_rename(string name) {
  if (tofiles.size()==0) return name;  // same name
  if (files.size()==0) {  // append prefix tofiles[0]
    int n=name.size();
    if (n>1 && name[1]==':') {  // remove : from drive letter
      if (n>2 && name[2]=='/') name=name.substr(0, 1)+name.substr(2), --n;
      else name[1]='/';
    }
    if (n>0 && name[0]!='/') name="/"+name;  // insert / if needed
    return tofiles[0]+name;
  }
  else {  // replace prefix files[i] with tofiles[i]
    const int n=name.size();
    for (int i=0; i<size(files) && i<size(tofiles); ++i) {
      const int fn=files[i].size();
      if (fn<=n && files[i]==name.substr(0, fn))
        return tofiles[i]+name.substr(fn);
    }
  }
  return name;
}
*/


void makepath(string path, int64_t date=0, int64_t attr=0);

#endif


