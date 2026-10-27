#include "recovery_reset_intent.h"
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
    const auto it=records.find(key); if (it==records.end()) return ReadResult::Missing;
    if (it->second.size()>size) return ReadResult::Fault;
    size=it->second.size(); memcpy(out,it->second.data(),size); return ReadResult::Found;
  }
  bool write(const char *key,const uint8_t *data,size_t size) override {
    ++writes; const bool fail=calls++==cut;
    if (!fail || uncertain) records[key]={data,data+size};
    return !fail;
  }
};
// Synthetic test keys only. Never use this generator in device code.
uint64_t seed=0x123456789abcdef1;
bool randomBytes(uint8_t *out,size_t size) {
  while(size--) { seed^=seed<<13; seed^=seed>>7; seed^=seed<<17; *out++=uint8_t(seed); }
  return true;
}
struct Fixture {
  Key secret{}; PublicKey trusted{}; Signature signature{};
  PreparedReceipt receipt{}; std::array<uint8_t,PreparedReceiptSize> body{};
  Proof oldMesh{}; Id secretRef{};
  ResetSafetySnapshot safe{true,true,true,true,true,true,true};
  Fixture() {
    secret[31]=1; assert(identityPublicKey(secret,trusted,randomBytes));
    receipt.operation.fill(1); receipt.device.fill(2); receipt.network.fill(3); receipt.binding.fill(4);
    assert(identityFingerprint(trusted,receipt.master));
    receipt.attempt=1; receipt.credentialEpoch=1; receipt.generation=42;
    receipt.oobCommitment.fill(5); receipt.challenge.fill(6); receipt.capabilities=15; receipt.budgetMs=15000;
    oldMesh.fill(7); secretRef.fill(8); sign();
  }
  void sign() {
    assert(encodePreparedReceipt(receipt,body.data()));
    assert(signPreparedReceipt(secret,receipt,signature,randomBytes));
  }
  IntentResult prepare(ResetIntentStore &store,const ResetSafetySnapshot &safety) const {
    return store.prepare(receipt,body.data(),body.size(),trusted,signature,oldMesh,secretRef,safety);
  }
  IntentResult commit(ResetIntentStore &store,const ResetSafetySnapshot &safety) const {
    return store.commit(receipt.operation,receipt.attempt,oldMesh,secretRef,safety);
  }
  IntentResult clear(ResetIntentStore &store) const {
    return store.observeMeshCleared(receipt.operation,receipt.attempt,true,safe);
  }
  IntentResult cancel(ResetIntentStore &store) const {
    return store.cancel(receipt.operation,receipt.attempt,oldMesh,safe);
  }
};
int main() {
  Fixture f;
  PreparedReceipt decoded{};
  assert(decodePreparedReceipt(f.body.data(),f.body.size(),decoded));
  uint8_t encoded[PreparedReceiptSize]{}; assert(encodePreparedReceipt(decoded,encoded));
  assert(!memcmp(encoded,f.body.data(),sizeof(encoded)));
  for (size_t n=0;n<f.body.size();++n) {
    assert(!decodePreparedReceipt(f.body.data(),n,decoded)); assert(decoded.generation==0);
  }
  for (size_t i=0;i<8;++i) {
    auto bad=f.body; bad[i]^=0x80; assert(!decodePreparedReceipt(bad.data(),bad.size(),decoded));
  }
  assert(!decodePreparedReceipt(nullptr,PreparedReceiptSize,decoded));
  puts("PASS reset intent: canonical receipt decode, reserved header and all truncations rejected");

  // Build all safety snapshots that omit exactly one of the seven checks.
  const std::vector<ResetSafetySnapshot> unsafe={
    {false,true,true,true,true,true,true}, {true,false,true,true,true,true,true},
    {true,true,false,true,true,true,true}, {true,true,true,false,true,true,true},
    {true,true,true,true,false,true,true}, {true,true,true,true,true,false,true},
    {true,true,true,true,true,true,false}};
  Memory mem; Journal journal(mem); ResetIntentStore live(journal);
  assert(live.load(f.receipt.device,1,41)==JournalState::Empty);
  for(const auto &snapshot:unsafe) assert(f.prepare(live,snapshot)==IntentResult::Rejected);
  assert(mem.writes==0);
  auto badSignature=f.signature; badSignature[0]^=1;
  assert(live.prepare(f.receipt,f.body.data(),f.body.size(),f.trusted,badSignature,f.oldMesh,f.secretRef,f.safe)==IntentResult::Rejected);
  assert(live.prepare(f.receipt,f.body.data(),f.body.size()-1,f.trusted,f.signature,f.oldMesh,f.secretRef,f.safe)==IntentResult::Rejected);
  Id zeroRef{}; Proof zeroMesh{};
  assert(live.prepare(f.receipt,f.body.data(),f.body.size(),f.trusted,f.signature,f.oldMesh,zeroRef,f.safe)==IntentResult::Rejected);
  assert(live.prepare(f.receipt,f.body.data(),f.body.size(),f.trusted,f.signature,zeroMesh,f.secretRef,f.safe)==IntentResult::Rejected);
  assert(mem.writes==0 && f.prepare(live,f.safe)==IntentResult::Applied);
  const Memory prepared=mem;
  assert(live.get()->phase==ResetPhase::Prepared && live.generationFloor()==42);
  const auto preparedWrites=mem.writes;
  assert(f.prepare(live,{})==IntentResult::Existing && mem.writes==preparedWrites);
  assert(f.clear(live)==IntentResult::Rejected);
  for(const auto &snapshot:unsafe) {
    assert(f.commit(live,snapshot)==IntentResult::Rejected);
    assert(live.cancel(f.receipt.operation,1,f.oldMesh,snapshot)==IntentResult::Rejected);
  }
  auto wrong=f.receipt.operation; wrong[0]^=1;
  assert(live.commit(wrong,1,f.oldMesh,f.secretRef,f.safe)==IntentResult::Rejected);
  assert(live.commit(f.receipt.operation,2,f.oldMesh,f.secretRef,f.safe)==IntentResult::Rejected);
  auto wrongMesh=f.oldMesh; wrongMesh[0]^=1;
  assert(live.commit(f.receipt.operation,1,wrongMesh,f.secretRef,f.safe)==IntentResult::Rejected);
  auto wrongRef=f.secretRef; wrongRef[0]^=1;
  assert(live.commit(f.receipt.operation,1,f.oldMesh,wrongRef,f.safe)==IntentResult::Rejected);
  assert(mem.writes==preparedWrites);
  Fixture other; other.receipt.operation[0]=9; other.receipt.generation=43; other.sign();
  assert(other.prepare(live,f.safe)==IntentResult::Rejected); // One active operation.
  assert(f.commit(live,f.safe)==IntentResult::Applied);
  const Memory committed=mem; const auto committedWrites=mem.writes;
  assert(f.commit(live,{})==IntentResult::Existing && f.prepare(live,{})==IntentResult::Existing);
  assert(mem.writes==committedWrites && f.cancel(live)==IntentResult::Rejected);
  assert(live.observeMeshCleared(f.receipt.operation,1,false,f.safe)==IntentResult::Rejected);
  for(const auto &snapshot:unsafe)
    assert(live.observeMeshCleared(f.receipt.operation,1,true,snapshot)==IntentResult::Rejected);
  assert(f.clear(live)==IntentResult::Applied && f.clear(live)==IntentResult::Existing);
  assert(f.cancel(live)==IntentResult::Rejected && other.prepare(live,f.safe)==IntentResult::Rejected);
  puts("PASS reset intent: seven safety gates, identity/context checks, no double commit and no post-commit cancellation");

  // Every journal IO boundary: slot write/readback and selector write/readback.
  // A write may persist despite returning an error. Never authorize SDK work from
  // the failed live instance, even when reboot discovers the new committed phase.
  for(int transition=0;transition<4;++transition) for(int cut=0;cut<4;++cut) for(bool uncertain:{false,true}) {
    Memory cutMemory;
    if (transition==1 || transition==3) cutMemory.records=prepared.records;
    if (transition==2) cutMemory.records=committed.records;
    Journal cutJournal(cutMemory); ResetIntentStore store(cutJournal);
    const auto before=store.load(f.receipt.device,1,41);
    assert(before==(transition==0?JournalState::Empty:JournalState::Ready));
    cutMemory.calls=0; cutMemory.cut=cut; cutMemory.uncertain=uncertain;
    const auto result=transition==0?f.prepare(store,f.safe):transition==1?f.commit(store,f.safe):
      transition==2?f.clear(store):f.cancel(store);
    assert(result==IntentResult::StorageFault && store.state()==JournalState::Fault && store.get()==nullptr);
    assert(f.commit(store,f.safe)==IntentResult::StorageFault);
    cutMemory.cut=-1;
    Journal rebootJournal(cutMemory); ResetIntentStore reboot(rebootJournal);
    const auto after=reboot.load(f.receipt.device,1,41);
    const bool newCommitted=cut==3 || (cut==2 && uncertain);
    if (transition==0 && !newCommitted) {
      assert(after==JournalState::Empty && reboot.generationFloor()==41);
    } else {
      assert(after==JournalState::Ready);
      const auto expected=newCommitted?(transition==0?ResetPhase::Prepared:transition==1?ResetPhase::ResetCommitted:
        transition==2?ResetPhase::MeshCleared:ResetPhase::Cancelled):
        (transition==2?ResetPhase::ResetCommitted:ResetPhase::Prepared);
      assert(reboot.get()->phase==expected && reboot.generationFloor()==42);
    }
  }
  puts("PASS reset intent: 32 power-loss/ambiguous-commit boundaries preserve only durable phases; live failure stays closed");

  // Cancel before clear, consuming this generation and operation ID forever.
  Memory cancelMemory=prepared; Journal cancelJournal(cancelMemory); ResetIntentStore cancelled(cancelJournal);
  assert(cancelled.load(f.receipt.device,1,41)==JournalState::Ready);
  assert(f.cancel(cancelled)==IntentResult::Applied && f.cancel(cancelled)==IntentResult::Existing);
  assert(f.prepare(cancelled,f.safe)==IntentResult::Rejected && f.commit(cancelled,f.safe)==IntentResult::Rejected);
  auto stale=other; stale.receipt.generation=42; stale.sign();
  assert(stale.prepare(cancelled,f.safe)==IntentResult::Rejected);
  assert(other.prepare(cancelled,f.safe)==IntentResult::Applied && cancelled.generationFloor()==43);
  // Older cancelled requests cannot return after another generation is reserved.
  assert(other.cancel(cancelled)==IntentResult::Applied && f.prepare(cancelled,f.safe)==IntentResult::Rejected);
  Memory limitMemory; Journal limitJournal(limitMemory); ResetIntentStore limit(limitJournal);
  auto last=f; last.receipt.generation=UINT64_MAX; last.sign();
  assert(limit.load(f.receipt.device,1,UINT64_MAX-1)==JournalState::Empty);
  assert(last.prepare(limit,f.safe)==IntentResult::Applied && last.cancel(limit)==IntentResult::Applied);
  assert(other.prepare(limit,f.safe)==IntentResult::Rejected);
  puts("PASS reset intent: cancellation tombstone, generation high-water mark and overflow rejection survive retries");

  for(const char *key:{"active","slot_a"}) for(size_t i=0;i<prepared.records.at(key).size();++i) {
    Memory corrupt=prepared; corrupt.records[key][i]^=0x80;
    Journal brokenJournal(corrupt); ResetIntentStore broken(brokenJournal);
    assert(broken.load(f.receipt.device,1,41)==JournalState::Fault);
    assert(broken.get()==nullptr && f.prepare(broken,f.safe)==IntentResult::StorageFault);
  }
  // Recompute CRC deliberately: schema/identity checks must still reject a
  // well-formed journal containing an invalid or mismatched intent payload.
  for(size_t i:{size_t(0),size_t(4),size_t(6),size_t(8),size_t(9),size_t(16),size_t(32),size_t(36),size_t(40),
                size_t(96),size_t(104),size_t(272),size_t(337),size_t(401)}) {
    Memory corrupt=prepared; Journal mutate(corrupt); assert(mutate.load()==JournalState::Ready);
    std::vector<uint8_t> payload(mutate.data(),mutate.data()+mutate.size()); payload[i]^=0x80;
    assert(mutate.save(payload.data(),payload.size()));
    Journal brokenJournal(corrupt); ResetIntentStore broken(brokenJournal);
    assert(broken.load(f.receipt.device,1,41)==JournalState::Fault && broken.get()==nullptr);
  }
  for(int mismatch=0;mismatch<3;++mismatch) {
    Memory copy=prepared; Journal j(copy); ResetIntentStore s(j); Id device=f.receipt.device;
    if(mismatch==0) device[0]^=1;
    assert(s.load(device,mismatch==1?2:1,mismatch==2?42:41)==JournalState::Fault);
  }
  puts("PASS reset intent: 456 persisted-byte corruptions, valid-CRC schema/proof mutations and wrong identity/floor fail closed");
}
