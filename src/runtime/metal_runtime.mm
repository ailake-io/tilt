#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

#include "runtime/metal_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace tilt::rt::metal {
void destroy(void* opaque);
namespace {

struct Buffer {
  id<MTLBuffer> value = nil;
  std::size_t bytes = 0;
};

struct State {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLLibrary> library = nil;
  id<MTLComputePipelineState> gemm = nil;
  id<MTLComputePipelineState> relu = nil;
  id<MTLComputePipelineState> gelu = nil;
  id<MTLComputePipelineState> add = nil;
  id<MTLComputePipelineState> conv2d = nil;
};

static const char* kSource = R"metal(
#include <metal_stdlib>
using namespace metal;
kernel void tilt_gemm(device const float* A [[buffer(0)]], device const float* B [[buffer(1)]],
                      device float* C [[buffer(2)]], constant uint* dims [[buffer(3)]],
                      uint2 gid [[thread_position_in_grid]]) {
  if (gid.x >= dims.z || gid.y >= dims.x) return;
  float sum = 0.0;
  for (uint p = 0; p < dims.y; ++p) sum += A[gid.y * dims.y + p] * B[p * dims.z + gid.x];
  C[gid.y * dims.z + gid.x] = sum;
}
kernel void tilt_relu(device float* D [[buffer(0)]], uint id [[thread_position_in_grid]],
                      constant uint& n [[buffer(1)]]) { if (id < n && D[id] < 0.0) D[id] = 0.0; }
kernel void tilt_gelu(device float* D [[buffer(0)]], uint id [[thread_position_in_grid]],
                      constant uint& n [[buffer(1)]]) {
  if (id < n) { float v = D[id]; D[id] = 0.5 * v * (1.0 + tanh(0.7978845608 * (v + 0.044715 * v * v * v))); }
}
kernel void tilt_add(device const float* A [[buffer(0)]], device const float* B [[buffer(1)]],
                     device float* C [[buffer(2)]], uint id [[thread_position_in_grid]],
                     constant uint& n [[buffer(3)]]) { if (id < n) C[id] = A[id] + B[id]; }
kernel void tilt_conv2d(device const float* X [[buffer(0)]], device const float* W [[buffer(1)]],
                        device float* Y [[buffer(2)]], constant uint* P [[buffer(3)]],
                        uint id [[thread_position_in_grid]]) {
  uint B=P[0], CI=P[1], H=P[2], Width=P[3], CO=P[4], KH=P[5], KW=P[6], OH=P[7], OW=P[8];
  uint Stride=P[9], Pad=P[10], Dilation=P[11]; uint total=B*CO*OH*OW; if (id>=total) return;
  uint col=id%OW, row=(id/OW)%OH, o=(id/(OW*OH))%CO, b=id/(OW*OH*CO); float acc=0.0;
  for(uint c=0;c<CI;++c) for(uint u=0;u<KH;++u) for(uint v=0;v<KW;++v) {
    int yy=int(row*Stride+u*Dilation)-int(Pad), xx=int(col*Stride+v*Dilation)-int(Pad);
    if(yy>=0 && yy<int(H) && xx>=0 && xx<int(Width))
      acc += X[((b*CI+c)*H+uint(yy))*Width+uint(xx)] * W[((o*CI+c)*KH+u)*KW+v];
  } Y[id]=acc;
}
)metal";

id<MTLComputePipelineState> pipeline(State* s, NSString* name) {
  NSError* error = nil;
  id<MTLFunction> fn = [s->library newFunctionWithName:name];
  if (!fn) return nil;
  id<MTLComputePipelineState> result = [s->device newComputePipelineStateWithFunction:fn error:&error];
  [fn release];
  return result;
}

bool run(State* s, id<MTLComputePipelineState> p, NSArray<id<MTLBuffer>>* buffers,
         const MTLSize grid, const MTLSize threads, void* output, std::size_t output_bytes) {
  if (!s || !p) return false;
  id<MTLCommandBuffer> command = [s->queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  for (NSUInteger i = 0; i < buffers.count; ++i) [encoder setBuffer:buffers[i] offset:0 atIndex:i];
  [encoder dispatchThreads:grid threadsPerThreadgroup:threads];
  [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
  if (command.status != MTLCommandBufferStatusCompleted) return false;
  if (output && output_bytes) std::memcpy(output, [buffers[2] contents], output_bytes);
  return true;
}

MTLSize thread_group(id<MTLComputePipelineState> p) {
  const NSUInteger w = std::max<NSUInteger>(1, std::min<NSUInteger>(p.threadExecutionWidth, 256));
  return MTLSizeMake(w, 1, 1);
}

}  // namespace

void* create() {
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) return nullptr;
  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:kSource];
  id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
  [source release];
  id<MTLCommandQueue> queue = [device newCommandQueue];
  if (!library || !queue) { [library release]; [queue release]; return nullptr; }
  auto* state = new State(); state->device = device; state->library = library; state->queue = queue;
  state->gemm = pipeline(state, @"tilt_gemm"); state->relu = pipeline(state, @"tilt_relu");
  state->gelu = pipeline(state, @"tilt_gelu"); state->add = pipeline(state, @"tilt_add");
  state->conv2d = pipeline(state, @"tilt_conv2d");
  if (!state->gemm || !state->relu || !state->gelu || !state->add) {
    destroy(state); return nullptr;
  }
  return state;
}

void destroy(void* opaque) {
  auto* s = static_cast<State*>(opaque); if (!s) return;
  [s->gemm release]; [s->relu release]; [s->gelu release]; [s->add release]; [s->conv2d release];
  [s->library release]; [s->queue release]; [s->device release]; delete s;
}

bool gemm(void* opaque, const float* a, const float* b, float* c, int m, int k, int n) {
  auto* s = static_cast<State*>(opaque); if (!s || !a || !b || !c || m < 0 || k < 0 || n < 0) return false;
  const std::size_t sa=sizeof(float)*std::size_t(m)*k, sb=sizeof(float)*std::size_t(k)*n, sc=sizeof(float)*std::size_t(m)*n;
  id<MTLBuffer> A=[s->device newBufferWithBytes:a length:sa options:MTLResourceStorageModeShared];
  id<MTLBuffer> B=[s->device newBufferWithBytes:b length:sb options:MTLResourceStorageModeShared];
  id<MTLBuffer> C=[s->device newBufferWithLength:sc options:MTLResourceStorageModeShared];
  uint32_t dims[3]={uint32_t(m),uint32_t(k),uint32_t(n)};
  id<MTLBuffer> D=[s->device newBufferWithBytes:dims length:sizeof(dims) options:MTLResourceStorageModeShared];
  NSArray* buffers=@[A,B,C,D]; bool ok=run(s,s->gemm,buffers,MTLSizeMake(n,m,1),thread_group(s->gemm),c,sc);
  [A release]; [B release]; [C release]; [D release]; return ok;
}

bool unary(void* opaque, float* data, std::size_t n, id<MTLComputePipelineState> p) {
  auto* s=static_cast<State*>(opaque); if(!s || !data || n>std::numeric_limits<uint32_t>::max()) return false;
  const std::size_t bytes=n*sizeof(float); id<MTLBuffer> D=[s->device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];
  uint32_t count=uint32_t(n); id<MTLBuffer> N=[s->device newBufferWithBytes:&count length:sizeof(count) options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer> command=[s->queue commandBuffer]; id<MTLComputeCommandEncoder> encoder=[command computeCommandEncoder];
  [encoder setBuffer:D offset:0 atIndex:0]; [encoder setBuffer:N offset:0 atIndex:1];
  [encoder dispatchThreads:MTLSizeMake(n,1,1) threadsPerThreadgroup:thread_group(p)]; [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
  bool ok=command.status==MTLCommandBufferStatusCompleted; if(ok) std::memcpy(data,[D contents],bytes); [D release]; [N release]; return ok;
}
bool relu(void* s,float* d,std::size_t n){return unary(s,d,n,static_cast<State*>(s)->relu);}
bool gelu(void* s,float* d,std::size_t n){return unary(s,d,n,static_cast<State*>(s)->gelu);}

bool add(void* opaque,const float* a,const float* b,float* c,std::size_t n){
  auto*s=static_cast<State*>(opaque);if(!s||!a||!b||!c||n>std::numeric_limits<uint32_t>::max())return false;std::size_t bytes=n*sizeof(float);
  id<MTLBuffer>A=[s->device newBufferWithBytes:a length:bytes options:MTLResourceStorageModeShared],B=[s->device newBufferWithBytes:b length:bytes options:MTLResourceStorageModeShared],C=[s->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];uint32_t count=uint32_t(n);id<MTLBuffer>N=[s->device newBufferWithBytes:&count length:4 options:MTLResourceStorageModeShared];
  id<MTLCommandBuffer>cmd=[s->queue commandBuffer];id<MTLComputeCommandEncoder>e=[cmd computeCommandEncoder];[e setBuffer:A offset:0 atIndex:0];[e setBuffer:B offset:0 atIndex:1];[e setBuffer:C offset:0 atIndex:2];[e setBuffer:N offset:0 atIndex:3];[e dispatchThreads:MTLSizeMake(n,1,1) threadsPerThreadgroup:thread_group(s->add)];[e endEncoding];[cmd commit];[cmd waitUntilCompleted];bool ok=cmd.status==MTLCommandBufferStatusCompleted;if(ok)std::memcpy(c,[C contents],bytes);[A release];[B release];[C release];[N release];return ok;
}

bool conv2d(void* opaque,const float* x,const float* w,float* y,int batch,int ci,int h,int width,
            int co,int kh,int kw,int oh,int ow,int stride,int pad,int dilation){
  auto*s=static_cast<State*>(opaque);if(!s||!s->conv2d||!x||!w||!y||batch<0||ci<0||h<0||width<0||co<0||kh<0||kw<0||oh<0||ow<0)return false;
  const std::size_t sx=sizeof(float)*std::size_t(batch)*ci*h*width,sw=sizeof(float)*std::size_t(co)*ci*kh*kw,sy=sizeof(float)*std::size_t(batch)*co*oh*ow;
  id<MTLBuffer>X=[s->device newBufferWithBytes:x length:sx options:MTLResourceStorageModeShared],W=[s->device newBufferWithBytes:w length:sw options:MTLResourceStorageModeShared],Y=[s->device newBufferWithLength:sy options:MTLResourceStorageModeShared];
  uint32_t p[12]={uint32_t(batch),uint32_t(ci),uint32_t(h),uint32_t(width),uint32_t(co),uint32_t(kh),uint32_t(kw),uint32_t(oh),uint32_t(ow),uint32_t(stride),uint32_t(pad),uint32_t(dilation)};id<MTLBuffer>P=[s->device newBufferWithBytes:p length:sizeof(p) options:MTLResourceStorageModeShared];
  NSArray*buffers=@[X,W,Y,P];bool ok=run(s,s->conv2d,buffers,MTLSizeMake(std::size_t(batch)*co*oh*ow,1,1),thread_group(s->conv2d),y,sy);[X release];[W release];[Y release];[P release];return ok;
}

bool upload(void* opaque,const float* data,std::size_t bytes,void*& handle){auto*s=static_cast<State*>(opaque);if(!s||!data||bytes==0)return false;auto*b=new Buffer();b->value=[s->device newBufferWithBytes:data length:bytes options:MTLResourceStorageModeShared];if(!b->value){delete b;return false;}b->bytes=bytes;handle=b;return true;}
bool download(void*,const void* handle,float* data,std::size_t bytes){auto*b=static_cast<const Buffer*>(handle);if(!b||!data||bytes>b->bytes)return false;std::memcpy(data,[b->value contents],bytes);return true;}
bool release_buffer(void*,void* handle){auto*b=static_cast<Buffer*>(handle);if(!b)return false;[b->value release];delete b;return true;}
bool gemm_resident(void* opaque,const void* ah,const void* bh,void*& ch,int m,int k,int n){
  auto*s=static_cast<State*>(opaque);auto*a=static_cast<const Buffer*>(ah);auto*b=static_cast<const Buffer*>(bh);if(!s||!a||!b||m<0||k<0||n<0)return false;std::size_t bytes=sizeof(float)*std::size_t(m)*n;auto*c=static_cast<Buffer*>(ch);if(!c||c->bytes<bytes){if(c)release_buffer(s,c);c=new Buffer();c->value=[s->device newBufferWithLength:bytes options:MTLResourceStorageModeShared];c->bytes=bytes;ch=c;}
  uint32_t dims[3]={uint32_t(m),uint32_t(k),uint32_t(n)};id<MTLBuffer>D=[s->device newBufferWithBytes:dims length:sizeof(dims) options:MTLResourceStorageModeShared];id<MTLCommandBuffer>cmd=[s->queue commandBuffer];id<MTLComputeCommandEncoder>e=[cmd computeCommandEncoder];[e setBuffer:a->value offset:0 atIndex:0];[e setBuffer:b->value offset:0 atIndex:1];[e setBuffer:c->value offset:0 atIndex:2];[e setBuffer:D offset:0 atIndex:3];[e dispatchThreads:MTLSizeMake(n,m,1) threadsPerThreadgroup:thread_group(s->gemm)];[e endEncoding];[cmd commit];[cmd waitUntilCompleted];[D release];return cmd.status==MTLCommandBufferStatusCompleted;
}

}  // namespace tilt::rt::metal
