import random
import numpy as np

#Implements legal moves without superko
class Board:
  EMPTY = 0
  BLACK = 1
  WHITE = 2
  WALL = 3

  ZOBRIST_STONE = [[],[],[],[]]
  ZOBRIST_PLA = []

  ZOBRIST_RAND = random.Random()
  ZOBRIST_RAND.seed(123987456)

  PASS_LOC = 0

  for i in range((19+1)*(19+2)+1):
    ZOBRIST_STONE[BLACK].append(ZOBRIST_RAND.getrandbits(64))
    ZOBRIST_STONE[WHITE].append(ZOBRIST_RAND.getrandbits(64))
  for i in range(4):
    ZOBRIST_PLA.append(ZOBRIST_RAND.getrandbits(64))

  def __init__(self,size,copy_other=None):
    if size < 2 or size > 39:
      raise ValueError("Invalid board size: " + str(size))
    self.size = size
    self.arrsize = (size+1)*(size+2)+1
    self.dy = size+1
    self.adj = [-self.dy,-1,1,self.dy]
    self.diag = [-self.dy-1,-self.dy+1,self.dy-1,self.dy+1]

    if copy_other is not None:
      self.pla = copy_other.pla
      self.board = np.copy(copy_other.board)
      self.group_head = np.copy(copy_other.group_head)
      self.group_stone_count = np.copy(copy_other.group_stone_count)
      self.group_liberty_count = np.copy(copy_other.group_liberty_count)
      self.group_next = np.copy(copy_other.group_next)
      self.group_prev = np.copy(copy_other.group_prev)
      self.zobrist = copy_other.zobrist
      self.simple_ko_point = copy_other.simple_ko_point
    else:
      self.pla = Board.BLACK
      self.board = np.zeros(shape=(self.arrsize), dtype=np.int8)
      self.group_head = np.zeros(shape=(self.arrsize), dtype=np.int16)
      self.group_stone_count = np.zeros(shape=(self.arrsize), dtype=np.int16)
      self.group_liberty_count = np.zeros(shape=(self.arrsize), dtype=np.int16)
      self.group_next = np.zeros(shape=(self.arrsize), dtype=np.int16)
      self.group_prev = np.zeros(shape=(self.arrsize), dtype=np.int16)
      self.zobrist = 0
      self.simple_ko_point = None

      for i in range(-1,size+1):
        self.board[self.loc(i,-1)] = Board.WALL
        self.board[self.loc(i,size)] = Board.WALL
        self.board[self.loc(-1,i)] = Board.WALL
        self.board[self.loc(size,i)] = Board.WALL

      #More-easily catch errors
      self.group_head[0] = -1
      self.group_next[0] = -1
      self.group_prev[0] = -1

  def copy(self):
    return Board(self.size,copy_other=self)

  @staticmethod
  def get_opp(pla):
    return 3-pla
  @staticmethod
  def loc_static(x,y,size):
    return (x+1) + (size+1)*(y+1)

  def loc(self,x,y):
    return (x+1) + self.dy*(y+1)
  def loc_x(self,loc):
    return (loc % self.dy)-1
  def loc_y(self,loc):
    return (loc // self.dy)-1

  def is_adjacent(self,loc1,loc2):
    return loc1 == loc2 + self.adj[0] or loc1 == loc2 + self.adj[1] or loc1 == loc2 + self.adj[2] or loc1 == loc2 + self.adj[3]

  def pos_zobrist(self):
    return self.zobrist
  def sit_zobrist(self):
    return self.zobrist ^ Board.ZOBRIST_PLA[self.pla]

  def num_liberties(self,loc):
    if self.board[loc] == Board.EMPTY or self.board[loc] == Board.WALL:
      return 0
    return self.group_liberty_count[self.group_head[loc]]

  def is_simple_eye(self,pla,loc):
    adj0 = loc + self.adj[0]
    adj1 = loc + self.adj[1]
    adj2 = loc + self.adj[2]
    adj3 = loc + self.adj[3]

    if (self.board[adj0] != pla and self.board[adj0] != Board.WALL) or \
       (self.board[adj1] != pla and self.board[adj1] != Board.WALL) or \
       (self.board[adj2] != pla and self.board[adj2] != Board.WALL) or \
       (self.board[adj3] != pla and self.board[adj3] != Board.WALL):
      return False

    opp = Board.get_opp(pla)
    opp_corners = 0
    diag0 = loc + self.diag[0]
    diag1 = loc + self.diag[1]
    diag2 = loc + self.diag[2]
    diag3 = loc + self.diag[3]
    if self.board[diag0] == opp:
      opp_corners += 1
    if self.board[diag1] == opp:
      opp_corners += 1
    if self.board[diag2] == opp:
      opp_corners += 1
    if self.board[diag3] == opp:
      opp_corners += 1

    if opp_corners >= 2:
      return False
    if opp_corners <= 0:
      return True

    against_wall = (
      self.board[adj0] == Board.WALL or \
      self.board[adj1] == Board.WALL or \
      self.board[adj2] == Board.WALL or \
      self.board[adj3] == Board.WALL
    )

    if against_wall:
      return False
    return True


  def would_be_legal(self,pla,loc):
    if pla != Board.BLACK and pla != Board.WHITE:
      return False
    if loc == Board.PASS_LOC:
      return True
    if not self.is_on_board(loc):
      return False
    if self.board[loc] != Board.EMPTY:
      return False
    if self.would_be_suicide(pla,loc):
      return False
    if loc == self.simple_ko_point:
      return False
    return True

  def would_be_suicide(self,pla,loc):
    adj0 = loc + self.adj[0]
    adj1 = loc + self.adj[1]
    adj2 = loc + self.adj[2]
    adj3 = loc + self.adj[3]

    opp = Board.get_opp(pla)

    #If empty or capture, then not suicide
    if self.board[adj0] == Board.EMPTY or (self.board[adj0] == opp and self.group_liberty_count[self.group_head[adj0]] == 1) or \
       self.board[adj1] == Board.EMPTY or (self.board[adj1] == opp and self.group_liberty_count[self.group_head[adj1]] == 1) or \
       self.board[adj2] == Board.EMPTY or (self.board[adj2] == opp and self.group_liberty_count[self.group_head[adj2]] == 1) or \
       self.board[adj3] == Board.EMPTY or (self.board[adj3] == opp and self.group_liberty_count[self.group_head[adj3]] == 1):
      return False
    #If connects to own stone with enough liberties, then not suicide
    if self.board[adj0] == pla and self.group_liberty_count[self.group_head[adj0]] > 1 or \
       self.board[adj1] == pla and self.group_liberty_count[self.group_head[adj1]] > 1 or \
       self.board[adj2] == pla and self.group_liberty_count[self.group_head[adj2]] > 1 or \
       self.board[adj3] == pla and self.group_liberty_count[self.group_head[adj3]] > 1:
      return False
    return True

  #Returns the number of liberties a new stone placed here would have, or maxLibs if it would be >= maxLibs.
  def get_liberties_after_play(self,pla,loc,maxLibs):
    opp = Board.get_opp(pla)
    libs = []
    capturedGroupHeads = []

    #First, count immediate liberties and groups that would be captured
    for i in range(4):
      adj = loc + self.adj[i]
      if self.board[adj] == Board.EMPTY:
        libs.append(adj)
        if len(libs) >= maxLibs:
          return maxLibs

      elif self.board[adj] == opp and self.num_liberties(adj) == 1:
        libs.append(adj)
        if len(libs) >= maxLibs:
          return maxLibs

        head = self.group_head[adj]
        if head not in capturedGroupHeads:
          capturedGroupHeads.append(head)

    def wouldBeEmpty(possibleLib):
      if self.board[possibleLib] == Board.EMPTY:
        return True
      elif self.board[possibleLib] == opp:
        return (self.group_head[possibleLib] in capturedGroupHeads)
      return False

    #Next, walk through all stones of all surrounding groups we would connect with and count liberties, avoiding overlap.
    connectingGroupHeads = []
    for i in range(4):
      adj = loc + self.adj[i]
      if self.board[adj] == pla:
        head = self.group_head[adj]
        if head not in connectingGroupHeads:
          connectingGroupHeads.append(head)

          cur = adj
          while True:
            for k in range(4):
              possibleLib = cur + self.adj[k]
              if possibleLib != loc and wouldBeEmpty(possibleLib) and possibleLib not in libs:
                libs.append(possibleLib)
                if len(libs) >= maxLibs:
                  return maxLibs

            cur = self.group_next[cur]
            if cur == adj:
              break

    return len(libs)


  def to_string(self):
    def get_piece(x,y):
      loc = self.loc(x,y)
      if self.board[loc] == Board.BLACK:
        return 'X '
      elif self.board[loc] == Board.WHITE:
        return 'O '
      elif (x == 3 or x == self.size/2 or x == self.size-1-3) and (y == 3 or y == self.size/2 or y == self.size-1-3):
        return '* '
      else:
        return '. '

    return "\n".join("".join(get_piece(x,y) for x in range(self.size)) for y in range(self.size))

  def to_liberty_string(self):
    def get_piece(x,y):
      loc = self.loc(x,y)
      if self.board[loc] == Board.BLACK or self.board[loc] == Board.WHITE:
        libs = self.group_liberty_count[self.group_head[loc]]
        if libs <= 9:
          return str(libs) + ' '
        else:
          return '@ '
      elif (x == 3 or x == self.size/2 or x == self.size-1-3) and (y == 3 or y == self.size/2 or y == self.size-1-3):
        return '* '
      else:
        return '. '

    return "\n".join("".join(get_piece(x,y) for x in range(self.size)) for y in range(self.size))

  def set_pla(self,pla):
    self.pla = pla
  def is_on_board(self,loc):
    return loc >= 0 and loc < self.arrsize and self.board[loc] != Board.WALL

  #Set a given location with error checking. Suicide setting allowed.
  def set_stone(self,pla,loc):
    if pla != Board.EMPTY and pla != Board.BLACK and pla != Board.WHITE:
      raise ValueError("Invalid pla for board.set")
    if not self.is_on_board(loc):
      raise ValueError("Invalid loc for board.set")

    if self.board[loc] == pla:
      pass
    elif self.board[loc] == Board.EMPTY:
      self.add_unsafe(pla,loc)
    elif pla == Board.EMPTY:
      self.remove_single_stone_unsafe(loc)
    else:
      self.remove_single_stone_unsafe(loc)
      self.add_unsafe(pla,loc)

    #Clear any ko restrictions
    self.simple_ko_point = None


  #Play a stone at the given location, with non-superko legality checking and updating the pla and simple ko point
  def play(self,pla,loc):
    if pla != Board.BLACK and pla != Board.WHITE:
      raise ValueError("Invalid pla for board.play")

    if loc != Board.PASS_LOC:
      if not self.is_on_board(loc):
        raise ValueError("Invalid loc for board.set")
      if self.board[loc] != Board.EMPTY:
        raise ValueError("Location is nonempty")
      if self.would_be_suicide(pla,loc):
        raise ValueError("Move would be illegal suicide")
      if loc == self.simple_ko_point:
        raise ValueError("Move would be illegal simple ko recapture")

    self.playUnsafe(pla,loc)

  def playUnsafe(self,pla,loc):
    if loc == Board.PASS_LOC:
      self.simple_ko_point = None
      self.pla = Board.get_opp(pla)
    else:
      self.add_unsafe(pla,loc)
      self.pla = Board.get_opp(pla)

  def playRecordedUnsafe(self,pla,loc):
    capDirs = []
    opp = Board.get_opp(pla)
    for i in range(4):
      adj = loc + self.adj[i]
      if self.board[adj] == opp and self.group_liberty_count[self.group_head[adj]] == 1:
        capDirs.append(i)

    self.playUnsafe(pla,loc)
    return (pla,loc,self.simple_ko_point,capDirs)

  def undo(self,record):
    (pla,loc,simple_ko_point,capDirs) = record
    opp = Board.get_opp(pla)

    self.simple_ko_point = simple_ko_point
    self.pla = pla

    if loc == Board.PASS_LOC:
      return

    #Re-fill stones in all captured directions
    for capdir in capDirs:
      adj = loc + self.adj[capdir]
      if self.board[adj] == Board.EMPTY:
        self.floodFillStones(opp,adj)

    #Delete the stone played here.
    self.zobrist ^= Board.ZOBRIST_STONE[pla][loc]
    self.board[loc] = Board.EMPTY

    #Zero out stuff in preparation for rebuilding
    head = self.group_head[loc]
    stone_count = self.group_stone_count[head]
    self.group_stone_count[head] = 0
    self.group_liberty_count[head] = 0

    #Uneat enemy liberties
    self.changeSurroundingLiberties(loc,Board.get_opp(pla),+1)

    #If this was not a single stone, we need to recompute the chain from scratch
    if stone_count > 1:
      #Run through the whole chain and make their heads point to nothing
      cur = loc
      while True:
        self.group_head[cur] = Board.PASS_LOC
        cur = self.group_next[cur]
        if cur == loc:
          break

      #Rebuild each chain adjacent now
      for i in range(4):
        adj = loc + self.adj[i]
        if self.board[adj] == pla and self.group_head[adj] == Board.PASS_LOC:
          self.rebuildChain(pla,adj)

    self.group_head[loc] = 0
    self.group_next[loc] = 0
    self.group_prev[loc] = 0


  #Add a chain of the given player to the given region of empty space, floodfilling it.
  #Assumes that this region does not border any chains of the desired color already
  def floodFillStones(self,pla,loc):
    head = loc
    self.group_liberty_count[head] = 0
    self.group_stone_count[head] = 0

    #Add a chain with links front <-> ... <-> head <-> head with all head pointers towards head
    front = self.floodFillStonesHelper(head, head, head, pla)

    #Now, we make head point to front, and that completes the circle!
    self.group_next[head] = front
    self.group_prev[front] = head

  #Floodfill a chain of the given color into this region of empty spaces
  #Make the specified loc the head for all the chains and updates the chainData of head with the number of stones.
  #Does NOT connect the stones into a circular list. Rather, it produces an linear linked list with the tail pointing
  #to tailTarget, and returns the head of the list. The tail is guaranteed to be loc.
  def floodFillStonesHelper(self, head, tailTarget, loc, pla):
    self.board[loc] = pla
    self.zobrist ^= Board.ZOBRIST_STONE[pla][loc]

    self.group_head[loc] = head
    self.group_stone_count[head] += 1
    self.group_next[loc] = tailTarget
    self.group_prev[tailTarget] = loc

    #Eat enemy liberties
    self.changeSurroundingLiberties(loc,Board.get_opp(pla),-1)

    #Recursively add stones around us.
    nextTailTarget = loc
    for i in range(4):
      adj = loc + self.adj[i]
      if self.board[adj] == Board.EMPTY:
        nextTailTarget = self.floodFillStonesHelper(head,nextTailTarget,adj,pla)
    return nextTailTarget

  #Floods through a chain of the specified player already on the board
  #rebuilding its links and counting its liberties as we go.
  #Requires that all their heads point towards
  #some invalid location, such as PASS_LOC or a location not of color.
  #The head of the chain will be loc.
  def rebuildChain(self,pla,loc):
    head = loc
    self.group_liberty_count[head] = 0
    self.group_stone_count[head] = 0

    #Rebuild chain with links front <-> ... <-> head <-> head with all head pointers towards head
    front = self.rebuildChainHelper(head, head, head, pla)

    #Now, we make head point to front, and that completes the circle!
    self.group_next[head] = front
    self.group_prev[front] = head


  #Does same thing as addChain, but floods through a chain of the specified color already on the board
  #rebuilding its links and also counts its liberties as we go. Requires that all their heads point towards
  #some invalid location, such as NULL_LOC or a location not of color.
  def rebuildChainHelper(self, head, tailTarget, loc, pla):
    #Count new liberties
    for dloc in self.adj:
      if self.board[loc+dloc] == Board.EMPTY and not self.is_group_adjacent(head,loc+dloc):
        self.group_liberty_count[head] += 1

    #Add stone here to the chain by setting its head
    self.group_head[loc] = head
    self.group_stone_count[head] += 1
    self.group_next[loc] = tailTarget
    self.group_prev[tailTarget] = loc

    #Recursively add stones around us.
    nextTailTarget = loc
    for i in range(4):
      adj = loc + self.adj[i]
      if self.board[adj] == pla and self.group_head[adj] != head:
        nextTailTarget = self.rebuildChainHelper(head,nextTailTarget,adj,pla)
    return nextTailTarget


  #Add a stone, assumes that the location is empty without checking
  def add_unsafe(self,pla,loc):
    opp = Board.get_opp(pla)

    #Put the stone down
    self.board[loc] = pla
    self.zobrist ^= Board.ZOBRIST_STONE[pla][loc]

    #Initialize the group for that stone
    self.group_head[loc] = loc
    self.group_stone_count[loc] = 1
    liberties = 0
    for dloc in self.adj:
      if self.board[loc+dloc] == Board.EMPTY:
        liberties += 1
    self.group_liberty_count[loc] = liberties
    self.group_next[loc] = loc
    self.group_prev[loc] = loc

    #Fill surrounding liberties of all adjacent groups
    #Carefully avoid doublecounting
    adj0 = loc + self.adj[0]
    adj1 = loc + self.adj[1]
    adj2 = loc + self.adj[2]
    adj3 = loc + self.adj[3]
    if self.board[adj0] == Board.BLACK or self.board[adj0] == Board.WHITE:
      self.group_liberty_count[self.group_head[adj0]] -= 1
    if self.board[adj1] == Board.BLACK or self.board[adj1] == Board.WHITE:
      if self.group_head[adj1] != self.group_head[adj0]:
        self.group_liberty_count[self.group_head[adj1]] -= 1
    if self.board[adj2] == Board.BLACK or self.board[adj2] == Board.WHITE:
      if self.group_head[adj2] != self.group_head[adj0] and \
         self.group_head[adj2] != self.group_head[adj1]:
        self.group_liberty_count[self.group_head[adj2]] -= 1
    if self.board[adj3] == Board.BLACK or self.board[adj3] == Board.WHITE:
      if self.group_head[adj3] != self.group_head[adj0] and \
         self.group_head[adj3] != self.group_head[adj1] and \
         self.group_head[adj3] != self.group_head[adj2]:
        self.group_liberty_count[self.group_head[adj3]] -= 1

    #Merge groups
    if self.board[adj0] == pla:
      self.merge_unsafe(loc,adj0)
    if self.board[adj1] == pla:
      self.merge_unsafe(loc,adj1)
    if self.board[adj2] == pla:
      self.merge_unsafe(loc,adj2)
    if self.board[adj3] == pla:
      self.merge_unsafe(loc,adj3)

    #Resolve captures
    opp_stones_captured = 0
    caploc = 0
    if self.board[adj0] == opp and self.group_liberty_count[self.group_head[adj0]] == 0:
      opp_stones_captured += self.group_stone_count[self.group_head[adj0]]
      caploc = adj0
      self.remove_unsafe(adj0)
    if self.board[adj1] == opp and self.group_liberty_count[self.group_head[adj1]] == 0:
      opp_stones_captured += self.group_stone_count[self.group_head[adj1]]
      caploc = adj1
      self.remove_unsafe(adj1)
    if self.board[adj2] == opp and self.group_liberty_count[self.group_head[adj2]] == 0:
      opp_stones_captured += self.group_stone_count[self.group_head[adj2]]
      caploc = adj2
      self.remove_unsafe(adj2)
    if self.board[adj3] == opp and self.group_liberty_count[self.group_head[adj3]] == 0:
      opp_stones_captured += self.group_stone_count[self.group_head[adj3]]
      caploc = adj3
      self.remove_unsafe(adj3)

    if self.group_liberty_count[self.group_head[loc]] == 0:
      self.remove_unsafe(loc)

    #Update ko point for legality checking
    if opp_stones_captured == 1 and \
       self.group_stone_count[self.group_head[loc]] == 1 and \
       self.group_liberty_count[self.group_head[loc]] == 1:
      self.simple_ko_point = caploc
    else:
      self.simple_ko_point = None

  #Apply the specified delta to the liberties of all adjacent groups of the specified color
  def changeSurroundingLiberties(self,loc,pla,delta):
    #Carefully avoid doublecounting
    adj0 = loc + self.adj[0]
    adj1 = loc + self.adj[1]
    adj2 = loc + self.adj[2]
    adj3 = loc + self.adj[3]
    if self.board[adj0] == pla:
      self.group_liberty_count[self.group_head[adj0]] += delta
    if self.board[adj1] == pla:
      if self.group_head[adj1] != self.group_head[adj0]:
        self.group_liberty_count[self.group_head[adj1]] += delta
    if self.board[adj2] == pla:
      if self.group_head[adj2] != self.group_head[adj0] and \
         self.group_head[adj2] != self.group_head[adj1]:
        self.group_liberty_count[self.group_head[adj2]] += delta
    if self.board[adj3] == pla:
      if self.group_head[adj3] != self.group_head[adj0] and \
         self.group_head[adj3] != self.group_head[adj1] and \
         self.group_head[adj3] != self.group_head[adj2]:
        self.group_liberty_count[self.group_head[adj3]] += delta

  def countImmediateLiberties(self,loc):
    adj0 = loc + self.adj[0]
    adj1 = loc + self.adj[1]
    adj2 = loc + self.adj[2]
    adj3 = loc + self.adj[3]
    count = 0
    if self.board[adj0] == Board.EMPTY:
      count += 1
    if self.board[adj1] == Board.EMPTY:
      count += 1
    if self.board[adj2] == Board.EMPTY:
      count += 1
    if self.board[adj3] == Board.EMPTY:
      count += 1
    return count

  def is_group_adjacent(self,head,loc):
    return (
      self.group_head[loc+self.adj[0]] == head or \
      self.group_head[loc+self.adj[1]] == head or \
      self.group_head[loc+self.adj[2]] == head or \
      self.group_head[loc+self.adj[3]] == head
    )

  #Helper, merge two groups assuming they're owned by the same player and adjacent
  def merge_unsafe(self,loc0,loc1):
    if self.group_stone_count[self.group_head[loc0]] >= self.group_stone_count[self.group_head[loc1]]:
      parent = loc0
      child = loc1
    else:
      child = loc0
      parent = loc1

    phead = self.group_head[parent]
    chead = self.group_head[child]
    if phead == chead:
      return

    #Walk the child group assigning the new head and simultaneously counting liberties
    new_stone_count = self.group_stone_count[phead] + self.group_stone_count[chead]
    new_liberties = self.group_liberty_count[phead]
    loc = child
    while True:
      adj0 = loc + self.adj[0]
      adj1 = loc + self.adj[1]
      adj2 = loc + self.adj[2]
      adj3 = loc + self.adj[3]

      #Any adjacent empty space is a new liberty as long as it isn't adjacent to the parent
      if self.board[adj0] == Board.EMPTY and not self.is_group_adjacent(phead,adj0):
        new_liberties += 1
      if self.board[adj1] == Board.EMPTY and not self.is_group_adjacent(phead,adj1):
        new_liberties += 1
      if self.board[adj2] == Board.EMPTY and not self.is_group_adjacent(phead,adj2):
        new_liberties += 1
      if self.board[adj3] == Board.EMPTY and not self.is_group_adjacent(phead,adj3):
        new_liberties += 1

      #Now assign the new parent head to take over the child (this also
      #prevents double-counting liberties)
      self.group_head[loc] = phead

      #Advance around the linked list
      loc = self.group_next[loc]
      if loc == child:
        break

    #Zero out the old head
    self.group_stone_count[chead] = 0
    self.group_liberty_count[chead] = 0

    #Update the new head
    self.group_stone_count[phead] = new_stone_count
    self.group_liberty_count[phead] = new_liberties

    #Combine the linked lists
    plast = self.group_prev[phead]
    clast = self.group_prev[chead]
    self.group_next[clast] = phead
    self.group_next[plast] = chead
    self.group_prev[chead] = plast
    self.group_prev[phead] = clast

  #Remove all stones in a group
  def remove_unsafe(self,group):
    head = self.group_head[group]
    pla = self.board[group]
    opp = Board.get_opp(pla)

    #Walk all the stones in the group and delete them
    loc = group
    while True:
      #Add a liberty to all surrounding opposing groups, taking care to avoid double counting
      adj0 = loc + self.adj[0]
      adj1 = loc + self.adj[1]
      adj2 = loc + self.adj[2]
      adj3 = loc + self.adj[3]
      if self.board[adj0] == opp:
        self.group_liberty_count[self.group_head[adj0]] += 1
      if self.board[adj1] == opp:
        if self.group_head[adj1] != self.group_head[adj0]:
          self.group_liberty_count[self.group_head[adj1]] += 1
      if self.board[adj2] == opp:
        if self.group_head[adj2] != self.group_head[adj0] and \
           self.group_head[adj2] != self.group_head[adj1]:
          self.group_liberty_count[self.group_head[adj2]] += 1
      if self.board[adj3] == opp:
        if self.group_head[adj3] != self.group_head[adj0] and \
           self.group_head[adj3] != self.group_head[adj1] and \
           self.group_head[adj3] != self.group_head[adj2]:
          self.group_liberty_count[self.group_head[adj3]] += 1

      next_loc = self.group_next[loc]

      #Zero out all the stuff
      self.board[loc] = Board.EMPTY
      self.zobrist ^= Board.ZOBRIST_STONE[opp][loc]
      self.group_head[loc] = 0
      self.group_next[loc] = 0
      self.group_prev[loc] = 0

      #Advance around the linked list
      loc = next_loc
      if loc == group:
        break

    #Zero out the head
    self.group_stone_count[head] = 0
    self.group_liberty_count[head] = 0

  #Remove a single stone
  def remove_single_stone_unsafe(self,rloc):
    pla = self.board[rloc]

    #Record all the stones in the group
    stones = []
    loc = rloc
    while True:
      stones.append(loc)
      loc = self.group_next[loc]
      if loc == rloc:
        break

    #Remove them all
    self.remove_unsafe(rloc)

    #Then add them back one by one
    for loc in stones:
      if loc != rloc:
        self.add_unsafe(pla,loc)


  #Helper, find liberties of group at loc. Fills in buf.
  def findLiberties(self, loc, buf):
    cur = loc
    while True:
      for i in range(4):
        lib = cur + self.adj[i]
        if self.board[lib] == Board.EMPTY:
          if lib not in buf:
            buf.append(lib)

      cur = self.group_next[cur]
      if cur == loc:
        break

  #Helper, find captures that gain liberties for the group at loc. Fills in buf
  def findLibertyGainingCaptures(self, loc, buf):
    pla = self.board[loc]
    opp = Board.get_opp(pla)

    #For performance, avoid checking for captures on any chain twice
    chainHeadsChecked = []

    cur = loc
    while True:
      for i in range(4):
        adj = cur + self.adj[i]
        if self.board[adj] == opp:
          head = self.group_head[adj]

          if self.group_liberty_count[head] == 1:
            if head not in chainHeadsChecked:
              #Capturing moves are precisely the liberties of the groups around us with 1 liberty.
              self.findLiberties(adj, buf)
              chainHeadsChecked.append(head)

      cur = self.group_next[cur]
      if cur == loc:
        break

  def searchIsLadderCaptured(self,loc,defenderFirst):
    if not self.is_on_board(loc):
      return False
    if self.board[loc] != Board.BLACK and self.board[loc] != Board.WHITE:
      return False

    if self.group_liberty_count[self.group_head[loc]] > 2 or (defenderFirst and self.group_liberty_count[self.group_head[loc]] > 1):
      return False

    #Make it so that pla is always the defender
    pla = self.board[loc]
    opp = Board.get_opp(pla)

    arrSize = self.size * self.size * 2 #A bit bigger due to paranoia about recaptures making the sequence longer.

    #Stack for the search. These are lists of possible moves to search at each level of the stack
    moveLists = [[] for i in range(arrSize)]
    moveListCur = [0 for i in range(arrSize)] #Current move list idx searched, equal to -1 if list has not been generated.
    records = [None for i in range(arrSize)] #Records so that we can undo moves as we search back up.
    stackIdx = 0

    moveLists[0] = []
    moveListCur[0] = -1

    returnValue = False
    returnedFromDeeper = False

    #debug = True

    while True:
      #if(debug) cout << ": " << stackIdx << " " << moveListCur[stackIdx] << " " << moveListStarts[stackIdx] << " " << moveListLens[stackIdx] << " " << returnValue << " " << returnedFromDeeper << endl;

      #Returned from the root - so that's the answer
      if stackIdx <= -1:
        assert(stackIdx == -1)
        return returnValue

      isDefender = (defenderFirst and (stackIdx % 2) == 0) or (not defenderFirst and (stackIdx % 2) == 1)

      #We just entered this level?
      if moveListCur[stackIdx] == -1:
        libs = self.group_liberty_count[self.group_head[loc]]

        #Base cases.
        #If we are the attacker and the group has only 1 liberty, we already win.
        if not isDefender and libs <= 1:
          returnValue = True
          returnedFromDeeper = True
          stackIdx -= 1
          continue

        #If we are the attacker and the group has 3 liberties, we already lose.
        if not isDefender and libs >= 3:
          returnValue = False
          returnedFromDeeper = True
          stackIdx -= 1
          continue

        #If we are the defender and the group has 2 liberties, we already win.
        if isDefender and libs >= 2:
          returnValue = False
          returnedFromDeeper = True
          stackIdx -= 1
          continue

        #If we are the defender and the attacker left a simple ko point, assume we already win
        #because we don't want to say yes on ladders that depend on kos
        #This should also hopefully prevent any possible infinite loops - I don't know of any infinite loop
        #that would come up in a continuous atari sequence that doesn't ever leave a simple ko point.
        if isDefender and self.simple_ko_point is not None:
          returnValue = False
          returnedFromDeeper = True
          stackIdx -= 1
          continue

        #Otherwise we need to keep searching.
        #Generate the move list. Attacker and defender generate moves on the group's liberties, but only the defender
        #generates moves on surrounding capturable opposing groups.
        if isDefender:
          moveLists[stackIdx] = []
          self.findLibertyGainingCaptures(loc,moveLists[stackIdx])
          self.findLiberties(loc,moveLists[stackIdx])
        else:
          moveLists[stackIdx] = []
          self.findLiberties(loc,moveLists[stackIdx])
          assert(len(moveLists[stackIdx]) == 2)

          #Early quitouts if the liberties are not adjacent
          #(so that filling one doesn't fill an immediate liberty of the other)
          move0 = moveLists[stackIdx][0]
          move1 = moveLists[stackIdx][1]
          if not self.is_adjacent(move0,move1):

            libs0 = self.countImmediateLiberties(move0)
            libs1 = self.countImmediateLiberties(move1)

            #We lose automatically if both escapes get the defender too many libs
            if libs0 >= 3 and libs1 >= 3:
              returnValue = False
              returnedFromDeeper = True
              stackIdx -= 1
              continue
            #Move 1 is not possible, so shrink the list
            elif libs0 >= 3:
              moveLists[stackIdx] = [move0]
            #Move 0 is not possible, so shrink the list
            elif libs1 >= 3:
              moveLists[stackIdx] = [move1]

        #And indicate to begin search on the first move generated.
        moveListCur[stackIdx] = 0

      #Else, we returned from a deeper level (or the same level, via illegal move)
      else:
        assert(moveListCur[stackIdx] >= 0)
        assert(moveListCur[stackIdx] < len(moveLists[stackIdx]))
        #If we returned from deeper we need to undo the move we made
        if returnedFromDeeper:
          self.undo(records[stackIdx])

        #Defender has a move that is not ladder captured?
        if isDefender and not returnValue:
          #Return! (returnValue is still false, as desired)
          returnedFromDeeper = True
          stackIdx -= 1
          continue

        #Attacker has a move that does ladder capture?
        if not isDefender and returnValue:
          #Return! (returnValue is still true, as desired)
          returnedFromDeeper = True
          stackIdx -= 1
          continue

        #Move on to the next move to search
        moveListCur[stackIdx] += 1

      #If there is no next move to search, then we lose.
      if moveListCur[stackIdx] >= len(moveLists[stackIdx]):
        #For a defender, that means a ladder capture.
        #For an attacker, that means no ladder capture found.
        returnValue = isDefender
        returnedFromDeeper = True
        stackIdx -= 1
        continue


      #Otherwise we do have an next move to search. Grab it.
      move = moveLists[stackIdx][moveListCur[stackIdx]]
      p = (pla if isDefender else opp)

      #if(print) cout << "play " << Location::getX(move,19) << " " << Location::getY(move,19) << " " << p << endl;

      #Illegal move - treat it the same as a failed move, but don't return up a level so that we
      #loop again and just try the next move.
      if not self.would_be_legal(p,move):
        returnValue = isDefender
        returnedFromDeeper = False
        #if(print) cout << "illegal " << endl;
        continue

      #Play and record the move!
      records[stackIdx] = self.playRecordedUnsafe(p,move)

      #And recurse to the next level
      stackIdx += 1
      moveListCur[stackIdx] = -1
      moveLists[stackIdx] = []

