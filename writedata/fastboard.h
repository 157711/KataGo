/*
 * fastboard.h
 * Originally from an unreleased project back in 2010, modified since.
 * Authors: brettharrison (original), David Wu (original and later modificationss).
 */

#ifndef FASTBOARD_H_
#define FASTBOARD_H_

#include "core/global.h"
#include "core/hash.h"

//TYPES AND CONSTANTS-----------------------------------------------------------------

//Player
typedef int8_t Player;
static const Player P_BLACK = 1;
static const Player P_WHITE = 2;

//Color of a point on the board
typedef int8_t Color;
static const Color C_EMPTY = 0;
static const Color C_BLACK = 1;
static const Color C_WHITE = 2;
static const Color C_WALL = 3;

//Conversions for players and colors
static inline Color getOpp(Color c)
{return c ^ 3;}
static inline char getCharOfColor(Color c)
{
  switch(c) {
  case C_BLACK: return 'X';
  case C_WHITE: return 'O';
  case C_EMPTY: return '.';
  default:  return '#';
  }
}

//Location of a point on the board
//(x,y) is represented as (x+1) + (y+1)*(x_size+1)
typedef short Loc;
namespace Location
{
  Loc getLoc(int x, int y, int x_size);
  int getX(Loc loc, int x_size);
  int getY(Loc loc, int x_size);

  void getAdjacentOffsets(short adj_offsets[8], int x_size);
  bool isAdjacent(Loc loc0, Loc loc1, int x_size);

  string toString(Loc loc, int x_size);

  int locToTensorPos(Loc loc, int bSize, int maxBSize);
  Loc tensorPosToLoc(int pos, int bSize, int maxBSize);
}

//Simple structure for storing moves. Not used below, but this is a convenient place to define it.
STRUCT_NAMED_PAIR(Loc,loc,Player,pla,Move);

//Fast lightweight board designed for playouts and simulations, where speed is essential.
//Undo, hashing, history, not supported. Simple ko rule only.
//Does not enforce player turn order.

struct FastBoard
{
  //Initialization------------------------------
  //Initialize the zobrist hash.
  //MUST BE CALLED AT PROGRAM START!
  static void initHash();

  //Board parameters and Constants----------------------------------------

  static const int MAX_LEN = 19;  //Maximum edge length allowed for the board
  static const int MAX_PLAY_SIZE = MAX_LEN * MAX_LEN;  //Maximum number of playable spaces
  static const int MAX_ARR_SIZE = (MAX_LEN+1)*(MAX_LEN+2)+1; //Maximum size of arrays needed

  //Location used to indicate an invalid spot on the board.
  static const Loc NULL_LOC = 0;
  //Location used to indicate a pass move is desired.
  static const Loc PASS_LOC = 1;

  //Zobrist Hashing------------------------------
  static bool IS_ZOBRIST_INITALIZED;
  static Hash128 ZOBRIST_SIZE_X_HASH[MAX_LEN+1];
  static Hash128 ZOBRIST_SIZE_Y_HASH[MAX_LEN+1];
  static Hash128 ZOBRIST_BOARD_HASH[MAX_ARR_SIZE][4];
  static Hash128 ZOBRIST_PLAYER_HASH[4];
  static Hash128 ZOBRIST_KO_LOC_HASH[MAX_ARR_SIZE];

  //Structs---------------------------------------

  //Tracks a chain/string/group of stones
  struct ChainData {
    Player owner;        //Owner of chain
    short num_locs;      //Number of stones in chain
    short num_liberties; //Number of liberties in chain
  };

  //Tracks locations for fast random selection
  struct PointList {
    PointList();
    PointList(const PointList&);
    void operator=(const PointList&);
    void add(Loc);
    void remove(Loc);
    int size() const;
    Loc& operator[](int);
    bool contains(Loc loc) const;

    Loc list_[MAX_PLAY_SIZE];   //Locations in the list
    int indices_[MAX_ARR_SIZE]; //Maps location to index in the list
    int size_;
  };

  //Move data passed back when moves are made to allow for undos
  struct MoveRecord {
    Player pla;
    Loc loc;
    Loc ko_loc;
    uint8_t capDirs; //First 4 bits indicate directions of capture, fifth bit indicates suicide
  };

  //Constructors---------------------------------
  FastBoard();  //Create FastBoard of size (19,19), multi-stone-suicide illegal
  FastBoard(int x, int y, bool multiStoneSuicideLegal); //Create Fastboard of size (x,y)
  FastBoard(const FastBoard& other);

  //Functions------------------------------------

  //Gets the number of liberties of the chain at loc. Assertion: location must be black or white.
  int getNumLiberties(Loc loc) const;
  //Returns the number of liberties a new stone placed here would have, or max if it would be >= max.
  int getNumLibertiesAfterPlay(Loc loc, Player pla, int max) const;
  //Gets the number of empty spaces directly adjacent to this location
  int getNumImmediateLiberties(Loc loc) const;

  //Check if moving here is would be a self-capture
  bool isSuicide(Loc loc, Player pla) const;
  //Check if moving here is would be an illegal self-capture
  bool isIllegalSuicide(Loc loc, Player pla) const;
  //Check if moving here is illegal due to simple ko
  bool isKoBanned(Loc loc) const;
  //Check if moving here is illegal.
  bool isLegal(Loc loc, Player pla) const;
  //Check if this location is on the board
  bool isOnBoard(Loc loc) const;
  //Check if this location contains a simple eye for the specified player.
  bool isSimpleEye(Loc loc, Player pla) const;
  //Check if a move at this location would be a capture in a simple ko mouth.
  bool wouldBeKoCapture(Loc loc, Player pla) const;

  //Configuration the board in various ways
  void setMultiStoneSuicideLegal(bool b);
  void clearSimpleKoLoc();

  //Sets the specified stone if possible. Returns true usually, returns false location or color were out of range.
  bool setStone(Loc loc, Color color);

  //Attempts to play the specified move. Returns true if successful, returns false if the move was illegal.
  bool playMove(Loc loc, Player pla);

  //Plays the specified move, assuming it is legal.
  void playMoveAssumeLegal(Loc loc, Player pla);

  //Plays the specified move, assuming it is legal, and returns a MoveRecord for the move
  MoveRecord playMoveRecorded(Loc loc, Player pla);

  //Undo the move given by record. Moves MUST be undone in the order they were made.
  //Undos will NOT typically restore the precise representation in the board to the way it was. The heads of chains
  //might change, the order of the circular lists might change, etc.
  void undo(MoveRecord record);

  //Get what the position hash would be if we were to play this move. Assumes the move is legal.
  Hash128 getPosHashAfterMove(Loc loc, Player pla) const;

  //Get a random legal move that does not fill a simple eye.
  Loc getRandomMCLegal(Player pla);

  //Check if the given stone is in unescapable atari or can be put into unescapable atari.
  //WILL perform a mutable search - may alter the linked lists or heads, etc.
  bool searchIsLadderCaptured(Loc loc, bool defenderFirst, vector<Loc>& buf);
  bool searchIsLadderCapturedAttackerFirst2Libs(Loc loc, vector<Loc>& buf, vector<Loc>& workingMoves);

  //For each player, mark all spots that are their pass-alive-group or pass-alive-territory
  //[result] must be a buffer of size MAX_ARR_SIZE and will get filled with the result
  void calculatePassAliveTerritory(Color* result) const;

  //Run some basic sanity checks on the board state, throws an exception if not consistent, for testing/debugging
  void checkConsistency() const;
  
  //Data--------------------------------------------

  int x_size;                  //Horizontal size of board
  int y_size;                  //Vertical size of board
  Color colors[MAX_ARR_SIZE];  //Color of each location on the board.

  //Every chain of stones has one of its stones arbitrarily designated as the head.
  ChainData chain_data[MAX_ARR_SIZE]; //For each head stone, the chaindata for the chain under that head. Undefined otherwise.
  Loc chain_head[MAX_ARR_SIZE];       //Where is the head of this chain? Undefined if EMPTY or WALL
  Loc next_in_chain[MAX_ARR_SIZE];    //Location of next stone in chain. Circular linked list. Undefined if EMPTY or WALL

  Loc ko_loc;   //A simple ko capture was made here, making it illegal to replay here next move

  PointList empty_list; //List of all empty locations on board

  Hash128 pos_hash; //A zobrist hash of the current board position (does not include ko point or player to move)

  short adj_offsets[8]; //Indices 0-3: Offsets to add for adjacent points. Indices 4-7: Offsets for diagonal points.

  //Rules
  bool isMultiStoneSuicideLegal; //Single-stone suicide is still always illegal.

  private:
  void init(int xS, int yS, bool multiStoneSuicideLegal);
  int countHeuristicConnectionLibertiesX2(Loc loc, Player pla) const;
  bool isLibertyOf(Loc loc, Loc head) const;
  void mergeChains(Loc loc1, Loc loc2);
  int removeChain(Loc loc);
  void removeSingleStone(Loc loc);

  void addChain(Loc loc, Player pla);
  Loc addChainHelper(Loc head, Loc tailTarget, Loc loc, Color color);
  void rebuildChain(Loc loc, Player pla);
  Loc rebuildChainHelper(Loc head, Loc tailTarget, Loc loc, Color color);
  void changeSurroundingLiberties(Loc loc, Color color, int delta);

  friend ostream& operator<<(ostream& out, const FastBoard& board);

  int findLiberties(Loc loc, vector<Loc>& buf, int bufStart, int bufIdx) const;
  int findLibertyGainingCaptures(Loc loc, vector<Loc>& buf, int bufStart, int bufIdx) const;

  void calculatePassAliveTerritoryForPla(Player pla, Color* result) const;


  //static void monteCarloOwner(Player player, FastBoard* board, int mc_counts[]);
};




#endif /* BOARD_H_ */
