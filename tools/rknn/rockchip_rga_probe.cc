#include <cstddef>
#include <im2d.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
static int dma_alloc(const char* heap, size_t bytes) {
  int h=open(heap,O_RDWR|O_CLOEXEC); if(h<0) return -1;
  dma_heap_allocation_data a{}; a.len=bytes; a.fd_flags=O_RDWR|O_CLOEXEC;
  int rc=ioctl(h,DMA_HEAP_IOCTL_ALLOC,&a); close(h); return rc==0 ? static_cast<int>(a.fd) : -1;
}
int main(){
  std::printf("RGA probe start\n");
  std::printf("api=%s\n", querystring(RGA_VERSION));
  int w=640,h=480; size_t yuv=static_cast<size_t>(w)*h*3/2, rgb=static_cast<size_t>(w)*h*3;
  int sfd=open("/dev/dma_heap/system-uncached-dma32",O_RDWR|O_CLOEXEC);
  if(sfd<0){perror("heap"); return 2;}
  std::printf("heap-open=%d (allocation requires dma_heap ioctl; testing virtual import)\n",sfd);
  void* srcmem=std::aligned_alloc(64,yuv); void* dstmem=std::aligned_alloc(64,rgb);
  std::memset(srcmem,128,yuv); std::memset(dstmem,0,rgb);
  rga_buffer_t src=wrapbuffer_virtualaddr_t(srcmem,w,h,w,h,RK_FORMAT_YCbCr_420_SP);
  rga_buffer_t dst=wrapbuffer_virtualaddr_t(dstmem,w,h,w,h,RK_FORMAT_BGR_888);
  IM_STATUS ck=imcheck(src,dst,{},{}); std::printf("virtual imcheck=%d %s\n",ck,imStrError_t(ck));
  IM_STATUS st=imcvtcolor(src,dst,RK_FORMAT_YCbCr_420_SP,RK_FORMAT_BGR_888,IM_COLOR_SPACE_DEFAULT);
  std::printf("virtual imcvtcolor=%d %s\n",st,imStrError_t(st));
  int sfd2=dma_alloc("/dev/dma_heap/system-uncached-dma32",yuv);
  int dfd2=dma_alloc("/dev/dma_heap/system-uncached-dma32",rgb);
  std::printf("dma-fds src=%d dst=%d\n",sfd2,dfd2);
  if(sfd2>=0 && dfd2>=0){
    void* sm=mmap(nullptr,yuv,PROT_READ|PROT_WRITE,MAP_SHARED,sfd2,0);
    void* dm=mmap(nullptr,rgb,PROT_READ|PROT_WRITE,MAP_SHARED,dfd2,0);
    if(sm!=MAP_FAILED && dm!=MAP_FAILED){ std::memset(sm,128,yuv); std::memset(dm,0,rgb);
      rga_buffer_t sf=wrapbuffer_fd_t(sfd2,w,h,w,h,RK_FORMAT_YCbCr_420_SP);
      rga_buffer_t df=wrapbuffer_fd_t(dfd2,w,h,w,h,RK_FORMAT_BGR_888);
      IM_STATUS fck=imcheck(sf,df,{},{}); IM_STATUS fst=imcvtcolor(sf,df,RK_FORMAT_YCbCr_420_SP,RK_FORMAT_BGR_888,IM_COLOR_SPACE_DEFAULT);
      std::printf("fd imcheck=%d %s\nfd imcvtcolor=%d %s\n",fck,imStrError_t(fck),fst,imStrError_t(fst));
    }
    if(sm!=MAP_FAILED) munmap(sm,yuv); if(dm!=MAP_FAILED) munmap(dm,rgb);
  }
  if(sfd2>=0) close(sfd2); if(dfd2>=0) close(dfd2);
  free(srcmem); free(dstmem); close(sfd); return st==IM_STATUS_SUCCESS?0:1;
}
