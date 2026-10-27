#include "recovery_attempt_secret.h"
#include "recovery_reset_intent.h"
#include "fixtures/recovery_attempt_vector.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace pump::recovery;

struct Memory final:BlobStore {
  std::map<std::string,std::vector<uint8_t>> records;
  int cut=-1,calls=0,writes=0; bool uncertain=false;
  ReadResult read(const char *key,uint8_t *out,size_t &size) override {
    if (calls++==cut) return ReadResult::Fault;
    const auto found=records.find(key); if (found==records.end()) return ReadResult::Missing;
    if (found->second.size()>size) return ReadResult::Fault;
    size=found->second.size(); memcpy(out,found->second.data(),size); return ReadResult::Found;
  }
  bool write(const char *key,const uint8_t *data,size_t size) override {
    ++writes; const bool fail=calls++==cut;
    if (!fail || uncertain) records[key]={data,data+size};
    return !fail;
  }
};
// Synthetic deterministic generator, deliberately not usable by production.
uint64_t seed=0x123456789abcdef1;
int randomMode=0;
bool randomBytes(uint8_t *out,size_t size) {
  if (randomMode==1) return false;
  if (randomMode==2) { memset(out,0,size); return true; }
  while(size--) { seed^=seed<<13; seed^=seed>>7; seed^=seed<<17; *out++=uint8_t(seed); }
  return true;
}
struct Fixture {
  Credential credential{}; PreparedReceipt receipt{}; AttemptOob oob{};
  Fixture() {
    credential.device.fill(2); credential.epoch=1;
    for (size_t i=0;i<credential.key.size();++i) credential.key[i]=uint8_t(i+1);
    receipt.operation.fill(1); receipt.device=credential.device; receipt.master.fill(3);
    receipt.network.fill(4); receipt.binding.fill(5); receipt.challenge.fill(6);
    receipt.attempt=7; receipt.credentialEpoch=1; receipt.generation=42; receipt.capabilities=15; receipt.budgetMs=15000;
    for (size_t i=0;i<oob.size();++i) oob[i]=uint8_t(0x80+i);
    assert(makeAttemptOobCommitment(receipt,oob,receipt.oobCommitment));
  }
};
bool zero(const AttemptOob &oob) { return oob==AttemptOob{}; }
void cannotResolve(AttemptSecretStore &store,const Id &reference,const PreparedReceipt &receipt) {
  AttemptOob out{}; out.fill(0xff); assert(!store.resolve(reference,receipt,out) && zero(out));
}
int main() {
  Fixture f;
  assert(!memcmp(f.receipt.oobCommitment.data(),AttemptCommitmentVector,32));
  Memory goldenMemory; Journal goldenJournal(goldenMemory); assert(goldenJournal.load()==JournalState::Empty);
  assert(goldenJournal.save(AttemptRecordVector,sizeof(AttemptRecordVector)));
  AttemptSecretStore golden(goldenJournal,randomBytes);
  assert(golden.load(f.credential)==JournalState::Ready);
  AttemptOob resolved{}; Id goldenReference{};
  for(size_t i=0;i<goldenReference.size();++i) goldenReference[i]=uint8_t(0x20+i);
  assert(golden.reference()==goldenReference && golden.resolve(goldenReference,f.receipt,resolved) && resolved==f.oob);
  puts("PASS attempt secret: independent Python HKDF/AES-GCM envelope and OOB commitment vectors");

  // Every immutable context field changes the OOB commitment. Challenge and
  // budget are instead bound by canonical full-receipt matching and AEAD.
  for(int field=0;field<8;++field) {
    auto changed=f.receipt;
    switch(field) {
      case 0: ++changed.operation[0]; break; case 1: ++changed.attempt; break;
      case 2: ++changed.device[0]; break; case 3: ++changed.credentialEpoch; break;
      case 4: ++changed.master[0]; break; case 5: ++changed.network[0]; break;
      case 6: ++changed.binding[0]; break; default: ++changed.generation;
    }
    Proof commitment{}; assert(makeAttemptOobCommitment(changed,f.oob,commitment));
    assert(commitment!=f.receipt.oobCommitment); cannotResolve(golden,goldenReference,changed);
  }
  auto changed=f.receipt; ++changed.challenge[0]; cannotResolve(golden,goldenReference,changed);
  changed=f.receipt; ++changed.budgetMs; cannotResolve(golden,goldenReference,changed);
  Id wrongRef=goldenReference; ++wrongRef[0]; cannotResolve(golden,wrongRef,f.receipt);
  const int initialWrites=goldenMemory.writes;
  assert(golden.install(f.receipt,f.oob)==AttemptSecretResult::Existing && goldenMemory.writes==initialWrites);
  assert(golden.install(changed,f.oob)==AttemptSecretResult::Rejected && goldenMemory.writes==initialWrites);
  puts("PASS attempt secret: complete intent/reference matching, exact idempotency, conflict refuses overwrite");

  Memory memory; Journal journal(memory); AttemptSecretStore store(journal,randomBytes);
  assert(store.load(f.credential)==JournalState::Empty);
  cannotResolve(store,{},f.receipt);
  auto bad=f.receipt; ++bad.oobCommitment[0];
  assert(store.install(bad,f.oob)==AttemptSecretResult::Rejected && memory.writes==0);
  assert(store.install(f.receipt,AttemptOob{})==AttemptSecretResult::Rejected && memory.writes==0);
  bad=f.receipt; bad.capabilities=0;
  assert(store.install(bad,f.oob)==AttemptSecretResult::Rejected && memory.writes==0);
  bad=f.receipt; bad.budgetMs=60001;
  assert(store.install(bad,f.oob)==AttemptSecretResult::Rejected && memory.writes==0);
  for(int mode:{1,2}) {
    randomMode=mode;
    assert(store.install(f.receipt,f.oob)==AttemptSecretResult::Rejected && memory.writes==0);
  }
  randomMode=0;
  assert(store.install(f.receipt,f.oob)==AttemptSecretResult::Applied);
  const Id reference=store.reference();
  assert(reference!=Id{} && store.resolve(reference,f.receipt,resolved) && resolved==f.oob);
  for(const auto &entry:memory.records) {
    assert(std::search(entry.second.begin(),entry.second.end(),f.oob.begin(),f.oob.end())==entry.second.end());
  }
  Journal rebootJournal(memory); AttemptSecretStore reboot(rebootJournal,randomBytes);
  assert(reboot.load(f.credential)==JournalState::Ready && reboot.reference()==reference);
  assert(reboot.resolve(reference,f.receipt,resolved) && resolved==f.oob);
  puts("PASS attempt secret: invalid input/RNG never writes, persisted OOB is encrypted, reboot restores same reference");

  for(int mismatch=0;mismatch<6;++mismatch) {
    auto credential=f.credential;
    switch(mismatch) {
      case 0: ++credential.device[0]; break; case 1: ++credential.epoch; break;
      case 2: ++credential.key[0]; break; case 3: credential.device={}; break;
      case 4: credential.epoch=0; break; default: credential.key={};
    }
    Memory copy=memory; Journal j(copy); AttemptSecretStore s(j,randomBytes);
    assert(s.load(credential)==JournalState::Fault && s.reference()==Id{});
    cannotResolve(s,reference,f.receipt);
    assert(s.install(f.receipt,f.oob)==AttemptSecretResult::StorageFault);
  }
  for(int cut=0;cut<4;++cut) for(bool uncertain:{false,true}) {
    Memory interrupted; Journal j(interrupted); AttemptSecretStore s(j,randomBytes);
    assert(s.load(f.credential)==JournalState::Empty);
    interrupted.calls=0; interrupted.cut=cut; interrupted.uncertain=uncertain;
    assert(s.install(f.receipt,f.oob)==AttemptSecretResult::StorageFault);
    assert(s.state()==JournalState::Fault && s.reference()==Id{}); cannotResolve(s,reference,f.receipt);
    interrupted.cut=-1;
    Journal afterJournal(interrupted); AttemptSecretStore after(afterJournal,randomBytes);
    const bool committed=cut==3 || (cut==2 && uncertain);
    assert(after.load(f.credential)==(committed?JournalState::Ready:JournalState::Empty));
    if (committed) assert(after.resolve(after.reference(),f.receipt,resolved) && resolved==f.oob);
    else cannotResolve(after,reference,f.receipt);
  }
  puts("PASS attempt secret: wrong credential rejected; 8 power-loss/ambiguous-commit boundaries stay closed");

  // Rebuild valid outer CRCs for each byte: rejection must come from the
  // encrypted schema/AAD/tag, rather than relying only on journal corruption.
  for(size_t i=0;i<AttemptSecretStore::RecordSize;++i) {
    Memory corrupt=memory; Journal mutate(corrupt); assert(mutate.load()==JournalState::Ready);
    std::vector<uint8_t> payload(mutate.data(),mutate.data()+mutate.size()); payload[i]^=0x80;
    assert(mutate.save(payload.data(),payload.size()));
    Journal j(corrupt); AttemptSecretStore s(j,randomBytes);
    assert(s.load(f.credential)==JournalState::Fault); cannotResolve(s,reference,f.receipt);
  }
  for(const char *key:{"active","slot_a"}) for(size_t i=0;i<memory.records.at(key).size();++i) {
    Memory corrupt=memory; corrupt.records[key][i]^=0x80;
    Journal j(corrupt); AttemptSecretStore s(j,randomBytes);
    assert(s.load(f.credential)==JournalState::Fault); cannotResolve(s,reference,f.receipt);
  }
  puts("PASS attempt secret: 272 authenticated-envelope byte mutations and 320 journal-byte corruptions rejected");

  // Persistence integration only: no SDK, no device IO, no authorization claim.
  Key signing{}; signing[31]=1; PublicKey pub{}; Signature signature{};
  assert(identityPublicKey(signing,pub,randomBytes) && identityFingerprint(pub,f.receipt.master));
  assert(makeAttemptOobCommitment(f.receipt,f.oob,f.receipt.oobCommitment));
  uint8_t body[PreparedReceiptSize]{};
  assert(encodePreparedReceipt(f.receipt,body) && signPreparedReceipt(signing,f.receipt,signature,randomBytes));
  Memory secrets,intents; Journal secretsJournal(secrets),intentJournal(intents);
  AttemptSecretStore durable(secretsJournal,randomBytes); ResetIntentStore intent(intentJournal);
  assert(durable.load(f.credential)==JournalState::Empty && intent.load(f.credential.device,1,41)==JournalState::Empty);
  assert(durable.install(f.receipt,f.oob)==AttemptSecretResult::Applied);
  Proof oldMesh{}; oldMesh.fill(9); ResetSafetySnapshot safe{true,true,true,true,true,true,true};
  assert(intent.prepare(f.receipt,body,sizeof(body),pub,signature,oldMesh,durable.reference(),safe)==IntentResult::Applied);
  assert(durable.resolve(intent.get()->attemptSecretRef,intent.get()->receipt,resolved) && resolved==f.oob);
  assert(intent.commit(f.receipt.operation,f.receipt.attempt,oldMesh,durable.reference(),safe)==IntentResult::Applied);
  puts("PASS attempt secret: durable secret reference resolves exactly the signed reset intent before commit");
}
