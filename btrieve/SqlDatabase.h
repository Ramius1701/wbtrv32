#ifndef __SQL_DATABASE_H_
#define __SQL_DATABASE_H_

#include <filesystem>
#include <memory>
#include <string>

#include "BtrieveDatabase.h"
#include "ErrorCode.h"
#include "LRUCache.h"
#include "OpenMode.h"
#include "OperationCode.h"
#include "Query.h"
#include "Text.h"

namespace btrieve {

class RecordLoader {
 public:
  virtual ~RecordLoader() = default;

  virtual BtrieveDatabase::LoadRecordResult onRecordLoaded(
      std::basic_string_view<uint8_t> record) = 0;

  virtual void onRecordsComplete() = 0;
};

// Identifies the Btrieve file a sql database was converted from, so a later
// open can tell whether that file has been replaced since (restored from a
// backup, reset by copying a blank file over it, upgraded, ...).
struct SourceFingerprint {
  uint64_t size = 0;
  int64_t modificationTime = 0;
  uint64_t hash = 0;  // FNV-1a over the whole file
};

// Who a database is: a unique id, also written in the marker file that stands
// in for its Btrieve file, and the name of that file. A marker that turns up
// under another name (renamed or copied by an application) is matched back to
// its database by the id.
// A file's owner name (Set Owner, 29) and what it allows without it:
// mode 0/2 = nothing, 1/3 = read-only access. (Modes 2/3 also asked Btrieve
// to encrypt the file; the database is not encrypted.)
struct OwnerInfo {
  std::string name;
  int mode = 0;
};

struct DatabaseIdentity {
  std::string id;
  std::string source;  // file name (no directory) of the Btrieve file
};

// An interface that abstracts a SQL-compatible implementation of
// BtrieveDatabase.
class SqlDatabase {
 public:
  SqlDatabase(unsigned int maxCacheSize)
      : recordLength(0),
        position(0),
        variableLengthRecords(false),
        cache(maxCacheSize) {}

  virtual ~SqlDatabase() = default;

  virtual const wchar_t *getFileExtension() = 0;

  // Opens a Btrieve database as a sql backed file.
  virtual BtrieveError open(const wchar_t *fileName,
                            OpenMode openMode = OpenMode::Normal) = 0;

  // Creates a new sql backed file using database as the source of records. If
  // fileName is nullptr, then it will be created in-memory.
  virtual std::unique_ptr<RecordLoader> create(
      const wchar_t *fileName, const BtrieveDatabase &database) = 0;

  // Closes an opened database.
  virtual void close() = 0;

  // Reads the fingerprint of the Btrieve file this database was converted
  // from. Returns false if none was recorded (databases created by Create, or
  // converted by an older version).
  virtual bool readSourceFingerprint(SourceFingerprint &fingerprint) {
    return false;
  }

  // Records the fingerprint of the Btrieve file this database holds the data
  // of. Returns false if it couldn't be written (e.g. opened read-only).
  virtual bool writeSourceFingerprint(const SourceFingerprint &fingerprint) {
    return false;
  }

  // Identity of the database file at path, read or written on a short-lived
  // connection of its own, so it works whatever mode the file is open in.
  virtual bool readIdentity(const std::filesystem::path &database,
                            DatabaseIdentity &identity) {
    return false;
  }
  virtual bool writeIdentity(const std::filesystem::path &database,
                             const DatabaseIdentity &identity) {
    return false;
  }

  // The file's owner, if it has one; writeOwner(nullptr) clears it.
  virtual bool readOwner(const std::filesystem::path &database,
                         OwnerInfo &owner) {
    return false;
  }
  virtual bool writeOwner(const std::filesystem::path &database,
                          const OwnerInfo *owner) {
    return false;
  }

  // An application's own transaction (Begin/End/Abort Transaction): every
  // change on this connection between begin and end is committed together,
  // or none is.
  virtual bool beginUserTransaction() { return false; }
  virtual bool endUserTransaction() { return false; }
  virtual bool abortUserTransaction() { return false; }

  // Copies a consistent snapshot of one database file to another (which may
  // be open elsewhere, including by this process).
  virtual bool copyDatabase(const std::filesystem::path &from,
                            const std::filesystem::path &to) {
    return false;
  }

  virtual unsigned int getRecordLength() const { return recordLength; }

  virtual bool isVariableLengthRecords() const { return variableLengthRecords; }

  virtual const std::vector<Key> &getKeys() const { return keys; }

  unsigned int getPosition() const { return position; }

  void setPosition(unsigned int position_) { position = position_; }

  std::pair<bool, Record> getRecord(unsigned int position) {
    std::shared_ptr<Record> data = cache.get(position);
    if (data) {
      return std::pair<bool, Record>(true, *data);
    }

    std::pair<bool, Record> ret = selectRecord(position);
    if (ret.first) {
      cache.cache(position, ret.second);
    }
    return ret;
  }

  virtual BtrieveError stepFirst() = 0;
  virtual BtrieveError stepLast() = 0;
  virtual BtrieveError stepPrevious() = 0;
  virtual BtrieveError stepNext() = 0;
  virtual unsigned int getRecordCount() const = 0;
  virtual BtrieveError deleteAll() = 0;
  virtual BtrieveError deleteRecord() = 0;
  virtual std::pair<BtrieveError, unsigned int> insertRecord(
      std::basic_string_view<uint8_t> record) = 0;
  virtual BtrieveError updateRecord(unsigned int offset,
                                    std::basic_string_view<uint8_t> record) = 0;

  virtual BtrieveError getByKeyFirst(Query *query) = 0;
  virtual BtrieveError getByKeyLast(Query *query) = 0;
  virtual BtrieveError getByKeyEqual(Query *query) = 0;
  virtual BtrieveError getByKeyGreater(Query *query) = 0;
  virtual BtrieveError getByKeyGreaterOrEqual(Query *query) = 0;
  virtual BtrieveError getByKeyLess(Query *query) = 0;
  virtual BtrieveError getByKeyLessOrEqual(Query *query) = 0;
  virtual BtrieveError getByKeyNext(Query *query) = 0;
  virtual BtrieveError getByKeyPrevious(Query *query) = 0;

  virtual std::unique_ptr<Query> newQuery(
      unsigned int position, const Key *key,
      std::basic_string_view<uint8_t> keyData) = 0;

  virtual std::unique_ptr<Query> logicalCurrencySeek(int keyNumber,
                                                     unsigned int position,
                                                     BtrieveError &error) = 0;

 protected:
  virtual std::pair<bool, Record> selectRecord(unsigned int position) = 0;

  unsigned int recordLength;
  unsigned int position;
  bool variableLengthRecords;
  std::vector<Key> keys;
  LRUCache<unsigned int, Record> cache;
};
}  // namespace btrieve

#endif
