#ifndef BOARDHISTORY_H_
#define BOARDHISTORY_H_

#include "../core/global.h"
#include "../core/hash.h"
#include "../game/board.h"
#include "../game/rules.h"

struct KoHashTable;

//A data structure enabling fast checking if a move would be illegal due to superko.
struct BoardHistory {
  Rules rules;

  //Chronological history of moves
  vector<Move> moveHistory;
  //Chronological history of hashes, including the latest board's hash.
  //Cleared on a pass if passes clear ko bans
  vector<Hash128> koHashHistory;
  int koHistoryLastClearedBeginningMoveIdx;

  //Did this board location ever have a stone there before, or was it ever played?
  //(Also includes locations of suicides)
  bool wasEverOccupiedOrPlayed[Board::MAX_ARR_SIZE];
  //Locations where the next player is not allowed to play due to superko
  bool superKoBanned[Board::MAX_ARR_SIZE];

  //Number of consecutive passes made that count for ending the game or phase
  int consecutiveEndingPasses;
  //All ko hashes that have occurred after player's pass
  vector<Hash128> hashesAfterBlackPass;
  vector<Hash128> hashesAfterWhitePass;

  //Encore phase 0,1,2 for territory scoring
  int encorePhase;
  //Ko-prohibited locations for territory scoring
  bool blackKoProhibited[Board::MAX_ARR_SIZE];
  bool whiteKoProhibited[Board::MAX_ARR_SIZE];
  Hash128 koProhibitHash;

  //Used to implement once-only rules for ko captures in encore
  STRUCT_NAMED_TRIPLE(Hash128,posHashBeforeMove,Loc,moveLoc,Player,movePla,EncoreKoCapture);
  vector<EncoreKoCapture> koCapturesInEncore;

  //State of the grid as of the start of encore phase 2 for territory scoring
  Color secondEncoreStartColors[Board::MAX_ARR_SIZE];

  //Amount that should be added to komi
  int whiteBonusScore;

  //Is the game supposed to be ended now?
  bool isGameFinished;
  //Winner of the game if the game is supposed to have ended now, C_EMPTY if it is a draw or isNoResult.
  Player winner;
  //Score difference of the game if the game is supposed to have ended now
  float finalWhiteMinusBlackScore;
  //True this game is supposed to be ended but there is no result
  bool isNoResult;

  BoardHistory();
  ~BoardHistory();

  BoardHistory(const Board& board, Player pla, const Rules& rules);

  BoardHistory(const BoardHistory& other);
  BoardHistory& operator=(const BoardHistory& other);

  BoardHistory(BoardHistory&& other) noexcept;
  BoardHistory& operator=(BoardHistory&& other) noexcept;

  void clear(const Board& board, Player pla, const Rules& rules);
  //Set only the komi field of the rules, does not clear history, but does clear game-over conditions,
  void setKomi(float newKomi);

  float currentSelfKomi(Player pla) const;

  //Check if a move on the board is legal, taking into account the full game state and superko
  bool isLegal(const Board& board, Loc moveLoc, Player movePla) const;

  //For all of the below, rootKoHashTable is optional and if provided will slightly speedup superko searches

  //Make a move on the board assuming it's legal and update all history and game state information as well.
  //In the main phase, should still do reasonable things if the move is still board.isLegal but violates superko, or moves
  //past when the game is supposed to be over, to allow for robustness when this code is being used for analysis in a way
  //that violates ko stuff.
  void makeBoardMoveAssumeLegal(Board& board, Loc moveLoc, Player movePla, const KoHashTable* rootKoHashTable);

private:
  bool koHashOccursInHistory(Hash128 koHash, const KoHashTable* rootKoHashTable) const;
  int numberOfKoHashOccurrencesInHistory(Hash128 koHash, const KoHashTable* rootKoHashTable) const;
  void setKoProhibited(Player pla, Loc loc, bool b);
  int countAreaScoreWhiteMinusBlack(const Board& board) const;
  int countTerritoryAreaScoreWhiteMinusBlack(const Board& board) const;
};

struct KoHashTable {
  uint16_t* idxTable;
  vector<Hash128> koHashHistorySortedByLowBits;
  int koHistoryLastClearedBeginningMoveIdx;

  static const int TABLE_SIZE = 1 << 10;
  static const uint64_t TABLE_MASK = TABLE_SIZE-1;

  KoHashTable();
  ~KoHashTable();

  KoHashTable(const KoHashTable& other) = delete;
  KoHashTable& operator=(const KoHashTable& other) = delete;
  KoHashTable(KoHashTable&& other) = delete;
  KoHashTable& operator=(KoHashTable&& other) = delete;

  size_t size() const;

  void recompute(const BoardHistory& history);
  bool containsHash(Hash128 hash) const;
  int numberOfOccurrencesOfHash(Hash128 hash) const;

};


#endif
