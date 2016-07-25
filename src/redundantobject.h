#ifndef REDUNDANT_OBJECT_H
#define REDUNDANT_OBJECT_H

#include "simulator.h"
#include "coord.h"
#include "grid.h"

class REDUNDANT_OBJECT_STATE : public STATE {
public:
  REDUNDANT_OBJECT_STATE() {
    ObjectPos.resize(NUM_REDUNDANT_OBJECT);
  }

  virtual ~REDUNDANT_OBJECT_STATE() {

  }

  COORD AgentPos;
  std::vector<COORD> ObjectPos;

  virtual size_t hash() const {
    using boost::hash_combine;

    // Start with a hash value of 0
    std::size_t seed = 0;

    // Modify 'seed' by XORing and bit-shifting in
    // one member of 'Key' after the other:
    hash_combine(seed, hash_value(AgentPos));
    hash_combine(seed, boost::hash_value(ObjectPos));

    // Return the result.
    return seed;
  }

  static const int NUM_REDUNDANT_OBJECT = 5;
};

class REDUNDANT_OBJECT : public SIMULATOR {
public:
  REDUNDANT_OBJECT(int size, bool state_abstraction);

  ~REDUNDANT_OBJECT();

  virtual STATE *Copy(const STATE &state) const;

  virtual void Validate(const STATE &state) const;

  virtual STATE *CreateStartState() const;

  virtual void FreeState(STATE *state) const;

  virtual bool Step(STATE &state, int action, int &observation,
                    double &reward) const;

  virtual void GenerateLegal(const STATE &state, std::vector<int> &legal) const;

  virtual void GeneratePreferred(const STATE &state, const HISTORY &history,
                                 std::vector<int> &legal) const;

  virtual bool LocalMove(STATE &state, const HISTORY &history, int stepObservation) const;

  virtual void DisplayBeliefs(const BELIEF_STATE &beliefState,
                              std::ostream &ostr) const;

  virtual void DisplayState(const STATE &state, std::ostream &ostr) const;

  virtual void DisplayObservation(const STATE &state, int observation,
                                  std::ostream &ostr) const;

  virtual void DisplayAction(int action, std::ostream &ostr) const;

  virtual int AbstractionFunction(const STATE &state) const {
    const REDUNDANT_OBJECT_STATE &rstate = safe_cast<const REDUNDANT_OBJECT_STATE &>(state);
    return GetObservation(rstate);
  }

protected:
  int GetObservation(const REDUNDANT_OBJECT_STATE &state) const;

  GRID<int> mGrid;
  COORD mStartPos;
  COORD mGoalPos;

private:
  mutable MEMORY_POOL<REDUNDANT_OBJECT_STATE> mMemoryPool;
};

#endif  // REDUNDANT_OBJECT_H
