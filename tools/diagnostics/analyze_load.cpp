#define NOMINMAX
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <array>
static std::uint32_t u32(const unsigned char* p) { std::uint32_t n; std::memcpy(&n,p,4); return n; }
int main(int argc,char**argv){
 if(argc!=2)return 2;
#ifdef _WIN32
 HANDLE f=CreateFileA(argv[1],GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(f==INVALID_HANDLE_VALUE)return 3;
 LARGE_INTEGER size;GetFileSizeEx(f,&size);HANDLE mapping=CreateFileMappingA(f,nullptr,PAGE_READONLY,0,0,nullptr);
 auto* data=static_cast<const unsigned char*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));
 const auto file_bytes = static_cast<std::uint64_t>(size.QuadPart);
 if(!data)return 4;
#else
 int f=open(argv[1],O_RDONLY);if(f<0)return 3;
 struct stat info{};if(fstat(f,&info)||info.st_size<=0){close(f);return 4;}
 const auto file_bytes=static_cast<std::uint64_t>(info.st_size);
 auto* data=static_cast<const unsigned char*>(mmap(nullptr,file_bytes,PROT_READ,MAP_PRIVATE,f,0));
 if(data==MAP_FAILED){close(f);return 4;}
#endif
 std::array<std::uint16_t,256> table{};
 for(unsigned i=0;i<256;++i){unsigned v=i;for(int j=0;j<8;++j)v=(v>>1)^((v&1)?0xa001:0);table[i]=static_cast<std::uint16_t>(v);}
 std::uint64_t packets=0,bytes=0,pids[256]{},badpid=0,badcrc5=0,badcrc16=0,timeback=0,last=0;
 std::uint64_t offset=0;
 while(offset<file_bytes){
  if(offset+12>file_bytes)return 5;
  auto*p=data+offset;auto type=u32(p),length=u32(p+4);
  if(length<12||(length&3)||offset+length>file_bytes||u32(p+length-4)!=length)return 6;
  if(type==6){
   if(length<32)return 7;auto n=u32(p+20);if(n==0||n>length-32)return 8;
   auto*q=p+28;auto pid=q[0];++pids[pid];++packets;bytes+=n;
   std::uint64_t stamp=(static_cast<std::uint64_t>(u32(p+12))<<32)|u32(p+16);
   if(stamp<last)++timeback;last=stamp;
   if(((pid^(pid>>4))&15)!=15)++badpid;
   unsigned kind=pid&15;
   if((kind==1||kind==9||kind==5||kind==13||kind==4)&&n==3){
    unsigned bits=q[1]|(q[2]<<8),crc=31;
    for(unsigned bit=0;bit<11;++bit){unsigned carry=(crc^(bits>>bit))&1;crc>>=1;if(carry)crc^=0x14;}
    if((crc^31)!=(q[2]>>3))++badcrc5;
   }
   if((kind==3||kind==11||kind==7||kind==15)&&n>=3){
    std::uint16_t crc=0xffff;
    for(unsigned i=1;i<n-2;++i)crc=(crc>>8)^table[(crc^q[i])&255];
    crc^=0xffff;if(crc!=(q[n-2]|(q[n-1]<<8)))++badcrc16;
   }
  }
  offset+=length;
 }
 std::printf("{\"file_bytes\":%llu,\"packets\":%llu,\"usb_bytes\":%llu,\"nak_packets\":%llu,\"invalid_pid\":%llu,\"bad_crc5\":%llu,\"bad_crc16\":%llu,\"backward_timestamps\":%llu,\"pids\":{",
  (unsigned long long)file_bytes,(unsigned long long)packets,(unsigned long long)bytes,(unsigned long long)pids[0x5a],
  (unsigned long long)badpid,(unsigned long long)badcrc5,(unsigned long long)badcrc16,(unsigned long long)timeback);
 bool comma=false;for(unsigned i=0;i<256;++i)if(pids[i]){std::printf("%s\"0x%02x\":%llu",comma?",":"",i,(unsigned long long)pids[i]);comma=true;}
 std::puts("}}");
#ifdef _WIN32
 UnmapViewOfFile(data);CloseHandle(mapping);CloseHandle(f);
#else
 munmap(const_cast<unsigned char*>(data),file_bytes);close(f);
#endif
 return badpid||badcrc5||badcrc16||timeback?1:0;
}
