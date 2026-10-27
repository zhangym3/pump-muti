#include "master_identity.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace pump::recovery;
struct Memory final: BlobStore {
  std::map<std::string,std::vector<uint8_t>> records;
  int cut=-1,calls=0; bool uncertain=false;
  ReadResult read(const char *key,uint8_t *out,size_t &size) override {
    if (calls++==cut) return ReadResult::Fault;
    auto it=records.find(key); if (it==records.end()) return ReadResult::Missing;
    if (it->second.size()>size) return ReadResult::Fault;
    size=it->second.size(); memcpy(out,it->second.data(),size); return ReadResult::Found;
  }
  bool write(const char *key,const uint8_t *data,size_t size) override {
    const bool fail=calls++==cut;
    if (!fail || uncertain) records[key]={data,data+size};
    return !fail;
  }
};
// Test-only deterministic generator. Production uses esp_fill_random.
uint64_t counter=0x123456789abcdef1;
bool randomBytes(uint8_t *out,size_t size) {
  while(size--) { counter^=counter<<13;counter^=counter>>7;counter^=counter<<17;*out++=uint8_t(counter); }
  return true;
}
bool brokenRandom(uint8_t *,size_t) { return false; }
void printHex(const char *label,const uint8_t *p,size_t size) {
  printf("VECTOR %s=",label);for(size_t i=0;i<size;++i)printf("%02x",p[i]);puts("");
}
int main() {
  Key privateOne{}; privateOne[31]=1; PublicKey pub{};
  assert(identityPublicKey(privateOne,pub,randomBytes));
  constexpr uint8_t generator[]={
    4,0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
    0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96,
    0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
    0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5};
  assert(!memcmp(pub.data(),generator,sizeof(generator)));
  Key zero{}; PublicKey invalid{};
  assert(!identityPublicKey(zero,invalid,randomBytes));
  assert(!identityPublicKey(privateOne,invalid,brokenRandom));
  Proof challenge{}; challenge.fill(0x23); Signature signature{};
  assert(identitySign(privateOne,challenge.data(),challenge.size(),signature,randomBytes));
  assert(identityVerify(pub,challenge.data(),challenge.size(),signature));
  // P-256 order minus low-S is an equivalent ECDSA signature, but noncanonical.
  auto high=signature;
  constexpr uint8_t order[]={0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51};
  int borrow=0; for(int i=31;i>=0;--i) {int value=order[i]-signature[32+i]-borrow;high[32+i]=uint8_t(value);borrow=value<0;}
  assert(!identityVerify(pub,challenge.data(),challenge.size(),high));
  for(size_t i=0;i<signature.size();++i) {
    auto bad=signature;bad[i]^=1;assert(!identityVerify(pub,challenge.data(),challenge.size(),bad));
  }
  invalid=pub;invalid[0]=3;Id fingerprint{};assert(!identityFingerprint(invalid,fingerprint));
  puts("PASS identity: standard P-256 generator, failed RNG, 64 signature mutations, low-S canonicalization");

  PreparedReceipt receipt;
  receipt.operation.fill(1);receipt.device.fill(2);receipt.network.fill(3);receipt.binding.fill(4);
  assert(identityFingerprint(pub,receipt.master));
  receipt.attempt=7;receipt.credentialEpoch=1;receipt.generation=42;
  receipt.oobCommitment.fill(5);receipt.challenge.fill(6);receipt.capabilities=15;receipt.budgetMs=15000;
  uint8_t body[PreparedReceiptSize]{};
  assert(encodePreparedReceipt(receipt,body));
  assert(signPreparedReceipt(privateOne,receipt,signature,randomBytes));
  assert(verifyPreparedReceipt(pub,receipt,body,sizeof(body),signature));
  for(size_t i=0;i<sizeof(body);++i) {
    body[i]^=1;assert(!verifyPreparedReceipt(pub,receipt,body,sizeof(body),signature));body[i]^=1;
  }
  auto stale=receipt;stale.challenge[0]^=1;assert(!verifyPreparedReceipt(pub,stale,body,sizeof(body),signature));
  stale=receipt;stale.attempt++;assert(!verifyPreparedReceipt(pub,stale,body,sizeof(body),signature));
  stale=receipt;stale.capabilities=7;assert(!signPreparedReceipt(privateOne,stale,signature,randomBytes));
  stale=receipt;stale.budgetMs=60001;assert(!encodePreparedReceipt(stale,body));
  assert(!verifyPreparedReceipt(pub,receipt,body,sizeof(body)-1,signature));
  assert(signPreparedReceipt(privateOne,receipt,signature,randomBytes));
  printHex("master_public",pub.data(),pub.size());
  printHex("prepared_body",body,sizeof(body));printHex("prepared_signature",signature.data(),signature.size());
  puts("PASS receipt: all 176 fields/bytes bound, stale challenge/attempt and incomplete capabilities refused");

  // Synthetic installation vector for Python, JVM and Android provider interoperability.
  Memory vectorMemory; Journal vectorJournal(vectorMemory);
  assert(vectorJournal.load()==JournalState::Empty);
  uint8_t vectorRecord[144]{};put32(vectorRecord,0x3144494d);vectorRecord[4]=1;
  memset(vectorRecord+8,9,16);put32(vectorRecord+24,1);
  memcpy(vectorRecord+28,privateOne.data(),32);memcpy(vectorRecord+60,pub.data(),65);
  memset(vectorRecord+125,3,16);assert(vectorJournal.save(vectorRecord,sizeof(vectorRecord)));
  MasterIdentityStore vectorStore(vectorJournal,randomBytes);Id vectorDevice{};vectorDevice.fill(9);
  assert(vectorStore.load(vectorDevice)==JournalState::Ready);
  Proof vectorChallenge{};vectorChallenge.fill(6);
  assert(vectorStore.prove(vectorChallenge,signature));
  const auto vectorIdentity=*vectorStore.get();
  assert(verifyMasterProof(vectorIdentity,vectorChallenge,signature));
  printHex("master_proof_challenge",vectorChallenge.data(),vectorChallenge.size());
  printHex("master_proof_signature",signature.data(),signature.size());
  auto wrongProofIdentity=vectorIdentity;wrongProofIdentity.epoch++;
  assert(!verifyMasterProof(wrongProofIdentity,vectorChallenge,signature));
  wrongProofIdentity=vectorIdentity;wrongProofIdentity.network[0]^=1;
  assert(!verifyMasterProof(wrongProofIdentity,vectorChallenge,signature));
  wrongProofIdentity=vectorIdentity;wrongProofIdentity.device[0]^=1;
  assert(!verifyMasterProof(wrongProofIdentity,vectorChallenge,signature));
  wrongProofIdentity=vectorIdentity;wrongProofIdentity.master[0]^=1;
  assert(!verifyMasterProof(wrongProofIdentity,vectorChallenge,signature));
  puts("PASS proof: canonical cross-platform installation vector; wrong epoch/network/device/fingerprint refused");

  Id device{};device.fill(0x45);
  Memory baseline;Journal journal(baseline);MasterIdentityStore store(journal,randomBytes);
  assert(store.load(device)==JournalState::Empty && !store.get());
  assert(!store.prove(challenge,signature));assert(store.initialize());
  const auto identity=*store.get();assert(identity.epoch==1);
  assert(!store.initialize());assert(store.prove(challenge,signature));
  assert(verifyMasterProof(identity,challenge,signature));
  challenge[0]^=1;assert(!verifyMasterProof(identity,challenge,signature));challenge[0]^=1;
  Journal freshJournal(baseline);MasterIdentityStore fresh(freshJournal,randomBytes);
  assert(fresh.load(device)==JournalState::Ready && fresh.get()->master==identity.master);
  assert(fresh.get()->network==identity.network && fresh.get()->publicKey==identity.publicKey);
  auto wrong=device;wrong[1]^=1;assert(fresh.load(wrong)==JournalState::Fault && !fresh.get() && !fresh.initialize());
  puts("PASS store: explicit first initialization, persistent identity, fresh challenge proof, wrong physical UUID refused");
  for(int cut=0;cut<4;++cut) for(bool uncertain:{false,true}) {
    Memory memory;Journal liveJournal(memory);MasterIdentityStore live(liveJournal,randomBytes);
    assert(live.load(device)==JournalState::Empty);memory.calls=0;memory.cut=cut;memory.uncertain=uncertain;
    assert(!live.initialize() && !live.get() && !live.initialize());
    memory.cut=-1;Journal rebootJournal(memory);MasterIdentityStore reboot(rebootJournal,randomBytes);
    const auto state=reboot.load(device);
    assert(state==(cut==3 || (cut==2 && uncertain)?JournalState::Ready:JournalState::Empty));
  }
  // Corrupt every byte of committed state without updating the journal checksum.
  for(const char *key:{"active","slot_a"}) for(size_t i=0;i<baseline.records.at(key).size();++i) {
    auto memory=baseline;memory.records[key][i]^=1;Journal j(memory);MasterIdentityStore fail(j,randomBytes);
    assert(fail.load(device)==JournalState::Fault && !fail.initialize() && !fail.get());
  }
  // Well-formed journal containing mismatched public/private keys is also refused.
  auto memory=baseline;Journal j(memory);assert(j.load()==JournalState::Ready);
  auto record=std::vector<uint8_t>(j.data(),j.data()+j.size());record[60+1]^=1;assert(j.save(record.data(),record.size()));
  Journal badJournal(memory);MasterIdentityStore bad(badJournal,randomBytes);
  assert(bad.load(device)==JournalState::Fault && !bad.initialize());
  puts("PASS store: 8 ambiguous-write boundaries, every record corruption, mismatched keys fail closed");
}
