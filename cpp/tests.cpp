
#include <sstream>
#include "core/global.h"
#include "core/rand.h"
#include "game/board.h"
#include "game/rules.h"
#include "game/boardhistory.h"
#include "tests/tests.h"

int main(int argc, const char* const* argv) {
  (void)argc;
  (void)argv;
  testAssert(sizeof(size_t) == 8);
  Board::initHash();

  Tests::runRandHashTests();
  Tests::runBoardBasicTests();
  Tests::runBoardAreaTests();

  Tests::runRulesTests();

  Tests::runBoardUndoTest();
  Tests::runBoardStressTest();

  cout << "All tests passed" << endl;
  return 0;
}
