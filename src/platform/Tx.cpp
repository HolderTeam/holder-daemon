#include "platform/Tx.h"

namespace holder::store {

Tx::Tx(Db& db) : db_(db) {
  db_.exec("BEGIN IMMEDIATE;");
}

Tx::~Tx() {
  if (!committed_) {
    try {
      db_.exec("ROLLBACK;");
    } catch (...) {
      // Destructors must not throw.
    }
  }
}

void Tx::commit() {
  db_.exec("COMMIT;");
  committed_ = true;
}

} // namespace holder::store
