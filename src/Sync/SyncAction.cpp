#include "Sync/SyncAction.h"

#include "Filesystem/PathUtil.h"

namespace bv {
namespace sync {

std::string DescribeAction(const SyncAction& a) {
    const std::string rel = pathutil::ToUtf8(a.relativePath);
    switch (a.op) {
        case SyncOp::FileCopy: return "Copia file: " + rel;
        case SyncOp::FileReplace: return "Sostituisci file: " + rel;
        case SyncOp::FileDelete: return "Elimina file: " + rel;
        case SyncOp::DirCreate: return "Crea cartella: " + rel;
        case SyncOp::DirDelete: return "Elimina cartella: " + rel;
        case SyncOp::LinkCreate: return "Crea link: " + rel;
        case SyncOp::LinkReplace: return "Sostituisci link: " + rel;
        case SyncOp::LinkDelete: return "Elimina link: " + rel;
    }
    return "Azione: " + rel;
}

} // namespace sync
} // namespace bv
