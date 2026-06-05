#include "zeromq_backend.h"

// Port computation helpers and ZMQ backend template classes are defined
// in zeromq_backend.h (header-only due to C++ template requirements).
//
// Serialization utilities have been replaced by the serialize()/deserialize()
// methods on the concrete message types in rpc/message_types.h.
