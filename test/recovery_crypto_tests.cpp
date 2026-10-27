#include "recovery_crypto.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace pump::recovery;
int main() {
  Key key{}; Challenge c{};c.credentialEpoch=7;
  for (size_t i=0;i<32;++i) { key[i]=uint8_t(i+1);c.clientNonce[i]=uint8_t(i+33);c.deviceNonce[i]=uint8_t(i+65); }
  for (size_t i=0;i<16;++i)c.device[i]=uint8_t(i+97);
  Proof device{},client{};assert(makeProof(key,c,true,device) && makeProof(key,c,false,client));
  assert(device!=client);
  SecureSession a,b;assert(a.authenticate(true,key,c,device));assert(b.authenticate(false,key,c,client));
  std::array<uint8_t,SecureSession::MaxPlain> plain{},decoded{};
  for (size_t i=0;i<plain.size();++i)plain[i]=uint8_t(i);
  std::array<uint8_t,SecureSession::MaxFrame> frame{};
  size_t n=frame.size();assert(a.seal(plain.data(),plain.size(),frame.data(),n));
  // Every byte is authenticated; mutation must not consume the valid sequence.
  for (size_t i=0;i<n;++i) {
    auto bad=frame;bad[i]^=1;size_t count=decoded.size();
    assert(!b.open(bad.data(),n,decoded.data(),count));
  }
  size_t count=decoded.size();assert(b.open(frame.data(),n,decoded.data(),count));
  assert(count==plain.size() && decoded==plain);
  count=decoded.size();assert(!b.open(frame.data(),n,decoded.data(),count));
  count=decoded.size();assert(!a.open(frame.data(),n,decoded.data(),count));
  n=frame.size();assert(b.seal(plain.data(),17,frame.data(),n));count=decoded.size();
  assert(a.open(frame.data(),n,decoded.data(),count) && count==17);
  puts("PASS crypto: 548 frame mutations, replay, reflection, two directions, maximum plaintext");
  // Bind each transcript field, including credential version and roles.
  for (unsigned field=0;field<5;++field) {
    auto wrong=c;
    if (field==0)wrong.device[0]^=1;else if (field==1)++wrong.credentialEpoch;
    else if (field==2)wrong.clientNonce[0]^=1;else if(field==3)wrong.deviceNonce[0]^=1;
    SecureSession denied;
    assert(!denied.authenticate(field==4?false:true,key,wrong,device));
    assert(!denied.ready());
  }
  auto wrongKey=key;wrongKey[0]^=1;SecureSession denied;
  assert(!denied.authenticate(true,wrongKey,c,device));
  Proof zeros{};assert(!denied.authenticate(true,key,c,zeros));
  auto invalid=c;invalid.deviceNonce=invalid.clientNonce;
  assert(!makeProof(key,invalid,true,zeros));
  c.deviceNonce[0]^=0x80;Proof fresh{};assert(makeProof(key,c,true,fresh));
  assert(!a.authenticate(true,key,c,device) && !a.ready());
  assert(a.authenticate(true,key,c,fresh));count=decoded.size();assert(!a.open(frame.data(),n,decoded.data(),count));
  a.clear();n=frame.size();assert(!a.seal(plain.data(),1,frame.data(),n));
  puts("PASS crypto: transcript/credential/role binding, wrong key, invalid nonces, session invalidation");
  // Emit deterministic interoperability vectors, no deployed credential involved.
  c.deviceNonce[0]^=0x80;assert(makeProof(key,c,true,device));assert(makeProof(key,c,false,client));
  assert(a.authenticate(true,key,c,device));n=frame.size();assert(a.seal(plain.data(),17,frame.data(),n));
  auto hex=[](const char *name,const uint8_t *bytes,size_t size) { printf("VECTOR %s=",name);for(size_t i=0;i<size;++i)printf("%02x",bytes[i]);puts(""); };
  hex("device_proof",device.data(),device.size());hex("client_proof",client.data(),client.size());hex("frame",frame.data(),n);
}
