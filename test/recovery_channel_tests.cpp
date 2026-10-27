#include "recovery_channel.h"
#include <cassert>
#include <cstdio>
using namespace pump::recovery;
namespace {
bool randomBytes(uint8_t *p,size_t n) { for(size_t i=0;i<n;++i) p[i]=uint8_t(65+i); return true; }
Credential credential() {
  Credential c; c.epoch=7;
  for(size_t i=0;i<16;++i) c.device[i]=uint8_t(97+i);
  for(size_t i=0;i<32;++i) c.key[i]=uint8_t(1+i);
  return c;
}
std::array<uint8_t,55> hello(const Credential &c,Challenge &challenge) {
  std::array<uint8_t,55> msg{}; msg[0]=1; put16(msg.data()+1,1);
  challenge.device=c.device; challenge.credentialEpoch=c.epoch;
  for(size_t i=0;i<32;++i) challenge.clientNonce[i]=uint8_t(33+i);
  memcpy(msg.data()+3,c.device.data(),16); put32(msg.data()+19,c.epoch);
  memcpy(msg.data()+23,challenge.clientNonce.data(),32); return msg;
}
void establish(RecoveryEndpoint &ep,SecureSession &client,uint64_t now) {
  const auto c=credential(); Challenge challenge; const auto first=hello(c,challenge);
  ep.configure(&c); assert(ep.connect(now));
  uint8_t response[RecoveryMessageMax]{}; size_t size=sizeof(response);
  assert(ep.handle(first.data(),first.size(),response,size,now+1)); assert(size==65 && response[0]==0x81);
  memcpy(challenge.deviceNonce.data(),response+1,32); Proof device{}; memcpy(device.data(),response+33,32);
  assert(client.authenticate(true,c.key,challenge,device));
  uint8_t auth[33]={2}; Proof proof{}; assert(makeProof(c.key,challenge,false,proof)); memcpy(auth+1,proof.data(),32);
  size=sizeof(response); assert(ep.handle(auth,sizeof(auth),response,size,now+2)); assert(response[0]==0x82);
  uint8_t plain[512]; size_t length=sizeof(plain);
  assert(client.open(response+1,size-1,plain,length));
  assert(length==9 && plain[0]==0x81 && get16(plain+1)==1 && get16(plain+3)==0 && get32(plain+5)==7);
}
void fragments() {
  for(uint16_t length=1;length<=RecoveryMessageMax;++length) {
    RecoveryInbox input; RecoveryOutbox output; uint8_t data[RecoveryMessageMax];
    for(size_t i=0;i<length;++i) data[i]=uint8_t(i);
    assert(output.set(1,data,length)); size_t offset=0;
    while(offset<length) {
      uint8_t pull[5]={2}; put16(pull+1,1); put16(pull+3,uint16_t(offset)); assert(output.pull(pull,5));
      uint8_t value[20],repeat[20]; const auto n=output.read(value);
      assert(n==output.read(repeat) && !memcmp(value,repeat,n)); // An unread/lost response can be read again.
      assert(input.push(value,n,100)); offset+=n-7;
    }
    assert(input.ready() && input.id()==1 && input.size()==length && !memcmp(input.data(),data,length));
    input.consume(); uint8_t stale[]={1,1,0,1,0,0,0,1}; assert(!input.push(stale,sizeof(stale),100));
    stale[1]=2; assert(input.push(stale,sizeof(stale),100)); assert(input.ready());
  }
  RecoveryInbox input; uint8_t part[20]={1,1,0,14,0,0,0};
  assert(input.push(part,20,1)); assert(!input.push(part,20,2)); // Duplicate offset rejected.
  input.reset(); assert(input.push(part,20,1)); part[5]=13;
  assert(input.expired(3001)); assert(!input.push(part,8,3001)); // Partial transfer never survives deadline.
  part[1]=2;part[5]=0;part[3]=1;assert(!input.push(part,20,3002)); // Beyond total.
  input.reset(); part[1]=0xff;part[2]=0xff;part[3]=1;
  assert(input.push(part,8,1));input.consume();part[1]=1;part[2]=0;assert(!input.push(part,8,2)); // No ID wrap.
}
void sessions() {
  RecoveryEndpoint ep(randomBytes); SecureSession client; establish(ep,client,100);
  uint8_t request[RecoveryMessageMax]={3},response[RecoveryMessageMax];size_t n=sizeof(request)-1;
  const uint8_t query=1;assert(client.seal(&query,1,request+1,n));++n;
  size_t out=sizeof(response);assert(ep.handle(request,n,response,out,104));assert(response[0]==0x83);
  uint8_t plain[512];size_t length=sizeof(plain);assert(client.open(response+1,out-1,plain,length));assert(length==9);
  // Replayed AEAD cannot be accepted, and a failed session is not resurrected by reconnect.
  out=sizeof(response);assert(!ep.handle(request,n,response,out,105));assert(!ep.connect(106));assert(ep.connect(2105));
  out=sizeof(response);assert(!ep.handle(request,n,response,out,2106));
  establish(ep,client,10000);
  const uint8_t unsupported[]={0x40,1,2,3};request[0]=3;n=sizeof(request)-1;
  assert(client.seal(unsupported,sizeof(unsupported),request+1,n));++n;out=sizeof(response);
  assert(ep.handle(request,n,response,out,10003));length=sizeof(plain);
  assert(client.open(response+1,out-1,plain,length));assert(length==2 && plain[0]==0xff && plain[1]==1);
  assert(ep.expired(25003)); assert(!ep.expired(25002));
  for(int mutation=0;mutation<55;++mutation) {
    RecoveryEndpoint bad(randomBytes);const auto c=credential();bad.configure(&c);assert(bad.connect(0));
    Challenge challenge;auto first=hello(c,challenge);first[size_t(mutation)]^=1;out=sizeof(response);
    const bool accepted=bad.handle(first.data(),first.size(),response,out,1);
    // Changing a random client nonce is legitimate; identity/version/epoch/type changes are not.
    assert(accepted==(mutation>=23));
  }
  RecoveryEndpoint empty(randomBytes);assert(!empty.connect(0));
  const auto c=credential();Challenge challenge;auto first=hello(c,challenge);
  empty.configure(&c);assert(empty.connect(0));out=sizeof(response);assert(empty.handle(first.data(),55,response,out,1));
  uint8_t wrong[33]={2};out=sizeof(response);assert(!empty.handle(wrong,33,response,out,2));assert(!empty.connect(3));
  empty.disconnect();assert(!empty.connect(4)); // Disconnect does not reset rate limiting.
}
}
int main() { fragments(); sessions(); puts("Recovery fragmentation/authenticated-status tests passed (all 549 lengths, replay, timeout, identity, rate limit)"); }
