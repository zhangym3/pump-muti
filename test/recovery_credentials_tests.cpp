#include "recovery_credentials.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace pump::recovery;
struct Memory final : BlobStore {
  std::map<std::string,std::vector<uint8_t>> records;
  int cut=-1,calls=0; bool ambiguous=false;
  ReadResult read(const char *key,uint8_t *out,size_t &size) override {
    if (calls++==cut) return ReadResult::Fault;
    const auto it=records.find(key); if (it==records.end()) return ReadResult::Missing;
    if (it->second.size()>size) return ReadResult::Fault;
    size=it->second.size(); memcpy(out,it->second.data(),size); return ReadResult::Found;
  }
  bool write(const char *key,const uint8_t *data,size_t size) override {
    const bool fail=calls++==cut;
    if (!fail || ambiguous) records[key]={data,data+size};
    return !fail;
  }
};
int main() {
  Id uuid{}; Key key{}; for(size_t i=0;i<uuid.size();++i) uuid[i]=uint8_t(i+1);
  for(size_t i=0;i<key.size();++i) key[i]=uint8_t(i+41);
  Memory mem; Journal j(mem); CredentialStore credentials(j);
  assert(credentials.load(uuid)==JournalState::Empty && !credentials.get());
  assert(!credentials.initialize(Key{})); assert(credentials.initialize(key));
  assert(credentials.get()->device==uuid && credentials.get()->key==key && credentials.get()->epoch==1);
  const auto records=mem.records;
  assert(!credentials.initialize(key));
  auto otherKey=key; otherKey[0]^=1; assert(!credentials.initialize(otherKey)); assert(mem.records==records);
  Journal restarted(mem); CredentialStore reopened(restarted);
  assert(reopened.load(uuid)==JournalState::Ready && reopened.get()->key==key);
  auto otherUuid=uuid; otherUuid[0]^=1;
  assert(reopened.load(otherUuid)==JournalState::Fault && !reopened.get() && !reopened.initialize(key));
  // Even CRC-valid records must pass schema, role, identity and credential validation.
  for (size_t offset : {size_t(0),size_t(4),size_t(5),size_t(6),size_t(7),size_t(8),size_t(60)}) {
    Memory bad; Journal writer(bad); assert(writer.load()==JournalState::Empty);
    auto payload=std::vector<uint8_t>(j.data(),j.data()+j.size()); payload[offset]^=0x80;
    assert(writer.save(payload.data(),payload.size())); CredentialStore reader(writer);
    assert(reader.load(uuid)==JournalState::Fault && !reader.get());
  }
  // A failed initialization can only reboot as uninitialized or exactly the committed key.
  for(int cut=0;cut<6;++cut) for(bool ambiguous : {false,true}) {
    Memory interrupted; Journal journal(interrupted); CredentialStore live(journal);
    assert(live.load(uuid)==JournalState::Empty);
    interrupted.calls=0;interrupted.cut=cut;interrupted.ambiguous=ambiguous;
    assert(!live.initialize(key)); assert(live.state()==JournalState::Fault && !live.get());
    interrupted.cut=-1; Journal reboot(interrupted); CredentialStore loaded(reboot);
    const auto state=loaded.load(uuid);
    assert(state==JournalState::Empty || state==JournalState::Ready);
    if(state==JournalState::Ready) assert(loaded.get()->key==key && loaded.get()->epoch==1);
  }
  puts("PASS credentials: immutable initialization, identity/schema validation, 12 interrupted/ambiguous I/O cases");
}
