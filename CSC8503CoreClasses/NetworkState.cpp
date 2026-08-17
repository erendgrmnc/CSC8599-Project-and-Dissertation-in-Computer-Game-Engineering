#ifdef USEGL
#include "NetworkState.h"

// Deliberately empty. NetworkState's constructor and destructor are both defaulted
// in the header so the type stays trivially copyable - see the comment there. This
// file is retained because it is listed in CMakePC.cmake and because a future
// non-trivial member would need its definitions back.
#endif
