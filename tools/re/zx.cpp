#include <zarchive/zarchivereader.h>
#include <cstdio>
#include <vector>
#include <string>
int main(int argc,char**argv){
  auto*r=ZArchiveReader::OpenFromFile(argv[1]); if(!r){puts("open fail");return 1;}
  std::string mode=argv[2];
  if(mode=="ls"){ auto h=r->LookUp(argv[3]); unsigned n=r->GetDirEntryCount(h);
    for(unsigned i=0;i<n;i++){ZArchiveReader::DirEntry e; r->GetDirEntry(h,i,e); printf("%s %s %llu\n",e.isDirectory?"D":"F",std::string(e.name).c_str(),(unsigned long long)e.size);} return 0;}
  auto h=r->LookUp(argv[3]); uint64_t sz=r->GetFileSize(h); FILE*f=fopen(argv[4],"wb"); std::vector<char> b(1<<24);
  for(uint64_t o=0;o<sz;){uint64_t n=r->ReadFromFile(h,o,b.size(),b.data()); if(!n)break; fwrite(b.data(),1,n,f); o+=n;} fclose(f); printf("%llu\n",(unsigned long long)sz);
}
