#include "mcts.h"
#include "testsimulator.h"
#include "statistic.h"
#include "boost/timer.hpp"
#include "distribution.h"

#include <math.h>
#include <algorithm>

using namespace std;
using namespace UTILS;

MCTS::PARAMS::PARAMS()
  : Verbose(0),
    MaxDepth(100),
    NumSimulations(1000),
    NumStartStates(1000),
    UseTransforms(true),
    UseParticleFilter(false),
    NumTransforms(0),
    MaxAttempts(0),
    ExplorationConstant(1.0),
    ReuseTree(false),
    ThompsonSampling(false),
    TimeOutPerAction(-1),
    MemorySize(-1) {}

MCTS::MCTS(const SIMULATOR &simulator, const PARAMS &params)
  : Simulator(simulator), Params(params), TreeDepth(0) {
  VNODE::NumChildren = Simulator.GetNumActions();
  QNODE::NumChildren = Simulator.GetNumObservations();

  STATE *state = Simulator.CreateStartState();  //可能的开始状态

  Root = ExpandNode(state, History);  //生成根节点并初始化
  Root->Beliefs().AddSample(state);

  for (int i = 1; i < Params.NumStartStates; i++) {
    Root->Beliefs().AddSample(
          Simulator.CreateStartState());  //生成初始信念空间（样本集合） —— 至此
    // Root 节点构造完毕
  }

  StatBeliefSize.Initialise();
  StatTreeSize.Initialise();
  StatPeakTreeDepth.Initialise();
  StatRedundantNodes.Initialise();

  assert(VNODE::GetNumAllocated() == 1);

  if (Params.Verbose >= 1) Simulator.DisplayBeliefs(Root->Beliefs(), cout);
}

MCTS::~MCTS() {
  if (Params.Verbose >= 1) {
    StatBeliefSize.Print("#Belief begin size", cout);
    StatTreeSize.Print("#Tree size", cout);
    StatPeakTreeDepth.Print("#Peak tree depth", cout);
    StatRedundantNodes.Print("#Redundant nodes rate", cout);
    StatNumSimulation.Print("#Num simulations", cout);
  }

  VNODE::Free(Root, Simulator);
  VNODE::FreeAll();

  assert(VNODE::GetNumAllocated() == 0);
}

double MCTS::UCB[UCB_N][UCB_n];
bool MCTS::InitialisedFastUCB = false;

void MCTS::InitFastUCB(double exploration)
{
  cout << "Initialising fast UCB table... ";

  for (int N = 0; N < UCB_N; ++N)
    for (int n = 0; n < UCB_n; ++n)
      if (n == 0)
        UCB[N][n] = Infinity;
      else
        UCB[N][n] = exploration * sqrt(log(N + 1) / n);

  cout << "done" << endl;
  InitialisedFastUCB = true;
}

inline double MCTS::FastUCB(int N, int n) const
{
  if (InitialisedFastUCB && N < UCB_N && n < UCB_n)
    return UCB[N][n];

  if (n == 0)
    return Infinity;
  else {
    return Params.ExplorationConstant * sqrt(log(N + 1) / n);
  }
}

bool MCTS::Update(int action, int observation, STATE & /*state*/)
{
  History.Add(action, observation, Params.MemorySize);  //更新历史
  BELIEF_STATE beliefs;

  // Find matching vnode from the rest of the tree
  QNODE &qnode = Root->Child(action);
  VNODE *vnode = qnode.Child(observation);

  if (vnode) {
    if (Params.Verbose >= 1)
      cout << "Matched " << vnode->Beliefs().GetNumSamples() << " states"
           << endl;
    beliefs.Copy(vnode->Beliefs(), Simulator);  //把 vnode 中的 belief 复制出来
  } else {
    if (Params.Verbose >= 1) cout << "No matching node found" << endl;
  }

  if (Params.Verbose >= 1) Simulator.DisplayBeliefs(beliefs, cout);

  if (Params.UseParticleFilter) {  //增加更多样本！
    ParticleFilter(beliefs);

    if (Params.Verbose >= 1) Simulator.DisplayBeliefs(beliefs, cout);
  }

  // Generate transformed states to avoid particle deprivation
  if (Params.UseTransforms) {
    AddTransforms(beliefs);  //增加“扰动”

    if (Params.Verbose >= 1) Simulator.DisplayBeliefs(beliefs, cout);
  }

  // If we still have no particles, fail
  if (beliefs.Empty() && (!vnode || vnode->Beliefs().Empty()))
    return false;  //样本不足

  // Find a state to initialise prior (only requires fully observed state)
  const STATE *state = 0;
  if (vnode && !vnode->Beliefs().Empty())
    state = vnode->Beliefs().GetSample();  //得到一个*可能*的状态，主要目的是用来初始化先验信息
  else
    state = beliefs.GetSample();

  if (vnode && Params.ReuseTree) {
    int size1 = VNODE::GetNumAllocated();
    VNODE::Free(Root, Simulator, vnode);
    int size2 = VNODE::GetNumAllocated();

    assert(size2 < size1);

    Root = vnode;
    Root->Beliefs().Free(Simulator);
  } else { // Delete old tree and create new root
    VNODE::Free(Root, Simulator);
    Root = ExpandNode(state, History);
  }

  Root->Beliefs() = beliefs;  //这里的 belief 是在搜索过程中产生的，没有显式进行 bayes 更新

  return true;
}

int MCTS::SelectAction() {
  Search();
  return ActionSelection(Root, true);
}

void MCTS::SearchImp() {
  int historyDepth = History.Size();

  STATE *state = Root->Beliefs().CreateSample(Simulator);  // 得到一个可能的状态样本 -- 只在根节点采样 Root Sampling
  Simulator.Validate(*state);
  TreeDepth = 0;
  PeakTreeDepth = 0;

  SimulateV(*state, Root);  //通过 Monte Carlo 方法得到 V 值
  if (Params.Verbose >= 3) DisplayValue(4, cout);

  StatPeakTreeDepth.Add(PeakTreeDepth);
  Simulator.FreeState(state);
  History.Truncate(historyDepth);
}

int MCTS::ActionSelection(VNODE* vnode, bool greedy) const
{
  return Params.ThompsonSampling? ThompsonSampling(vnode, !greedy): GreedyUCB(vnode, !greedy);
}

int MCTS::GreedyUCB(VNODE* vnode, bool ucb) const //argmax_a {Q[a]}
{
  static std::vector<int> besta;
  besta.clear();
  double bestq = -Infinity;
  int N = vnode->Value.GetCount();

  for (int action = 0; action < Simulator.GetNumActions(); action++) {
    double q;
    int n;

    QNODE& qnode = vnode->Child(action);

    if (!qnode.Applicable()) {  //非合法动作
      continue;
    }

    q = qnode.Value.GetValue();
    n = qnode.Value.GetCount();
    if (ucb) {
      q += FastUCB(N, n);
    }

    if (q >= bestq)
    {
      if (q > bestq)
        besta.clear();
      bestq = q;
      besta.push_back(action);
    }
  }

  assert(!besta.empty());
  return besta[SimpleRNG::ins().Random(besta.size())];
}

void MCTS::Search() {
  assert(Root);
  StatBeliefSize.Add(Root->Beliefs().GetNumSamples());

  if (Params.TimeOutPerAction > 0.0) {  // Anytime mode
    boost::timer timer;
    int i = 0;

    while (1) {
      i += 1;
      SearchImp();

      if (timer.elapsed() > Params.TimeOutPerAction) {
        StatNumSimulation.Add(i);
        break;
      }
    }
  } else {
    for (int i = 0; i < Params.NumSimulations; i++)  //总共仿真（迭代）次数
    {
      SearchImp();
    }
  }

  StatTreeSize.Add(VNODE::GetNumAllocated());
}

double MCTS::SimulateV(STATE &state, VNODE *vnode) {
  int action = ActionSelection(vnode, false);

  PeakTreeDepth = max(PeakTreeDepth, TreeDepth);
  if (TreeDepth >= Params.MaxDepth) {  // search horizon reached
    return 0.0;
  }

  if (TreeDepth >= 1) {
    AddSample(vnode, state);  // state 加入到 vnode 对应的 belief 里去
  }

  QNODE &qnode = vnode->Child(action);
  double totalReward = SimulateQ(state, qnode, action);  //通过 Monte Carlo 方法得到 Q 值

  if (Params.ThompsonSampling) {
    vnode->GetCumulativeReward(state).Add(totalReward);
  }
  else {
    vnode->Value.Add(totalReward);
  }

  return totalReward;  // Return(s, pi(s))
}

double MCTS::SimulateQ(STATE &state, QNODE &qnode, int action) {
  int observation;
  double immediateReward;
  double delayedReward = 0.0;

  bool terminal = Simulator.Step(state, action, observation, immediateReward);  //一步模拟
  if (Params.ThompsonSampling) {
    qnode.Update(observation, immediateReward, 1);  //记录一次转移
  }

  assert(observation >= 0 && observation < Simulator.GetNumObservations());
  History.Add(action, observation, Params.MemorySize);

  if (Params.Verbose >= 3) {
    Simulator.DisplayAction(action, cout);
    Simulator.DisplayObservation(state, observation, cout);
    Simulator.DisplayReward(immediateReward, cout);
    Simulator.DisplayState(state, cout);
  }

  VNODE *&vnode = qnode.Child(observation);
  if (!vnode) {  // try to retrieve from belief pool
    size_t belief_hash = History.BeliefHash();
    if (Params.MemorySize >= 0 && History.Size() >= Params.MemorySize
        && VNODE::BeliefPool.count(belief_hash)) {
      StatRedundantNodes.Add(1.0);
      vnode = VNODE::BeliefPool[belief_hash];
      assert(vnode->GetBeliefHash() == belief_hash);
    }
    else {
      StatRedundantNodes.Add(0.0);
    }
  }

  if (!terminal) {
    TreeDepth++;
    if (vnode) {  //已经在树上
      delayedReward = SimulateV(state, vnode);
    } else {                       //叶子节点
      vnode = ExpandNode(&state, History);  //构造一个新节点

      STATE *copy = Simulator.Copy(state);
      delayedReward = Rollout(*copy);
      Simulator.FreeState(copy);

      if (Params.ThompsonSampling) {
        vnode->GetCumulativeReward(state).Add(delayedReward);
      }
      else {
        vnode->Value.Add(delayedReward);
      }
    }
    TreeDepth--;
  } else {
    if (!vnode) {
      vnode = ExpandNode(&state, History);  //终端节点
    }

    if (Params.ThompsonSampling) {
      vnode->GetCumulativeReward(state).Add(0.0);
    }
    else {
      vnode->Value.Add(0.0);
    }
  }

  double totalReward = immediateReward + Simulator.GetDiscount() * delayedReward;
  if (!Params.ThompsonSampling) {
    qnode.Value.Add(totalReward);
  }

  return totalReward;
}

VNODE *MCTS::ExpandNode(const STATE *state, HISTORY &history) {
  VNODE *vnode = VNODE::Create(history, Params.MemorySize);
  vnode->Value.Set(0, 0);
  Simulator.Prior(state, history, vnode);  //设置先验信息
  return vnode;
}

void MCTS::AddSample(VNODE *node, const STATE &state) {
  STATE *sample = Simulator.Copy(state);
  node->Beliefs().AddSample(sample);
}

int MCTS::ThompsonSampling(VNODE *vnode, bool sampling) const {
  vector<int> unexplored_actions;

  for (int action = 0; action < Simulator.GetNumActions(); action++) {
    QNODE &qnode = vnode->Child(action);

    if (!qnode.Applicable()) {  //非合法动作
      continue;
    }

    if (qnode.GetCount() <= 0) {
      unexplored_actions.push_back(action);
    }
  }

  if (!unexplored_actions.empty()) {
    return unexplored_actions[SimpleRNG::ins().Random(
        unexplored_actions.size())];
  }

  int besta = -1;
  double bestq = -Infinity;

  for (int action = 0; action < Simulator.GetNumActions(); action++) {
    QNODE &qnode = vnode->Child(action);

    if (!qnode.Applicable()) {  //非合法动作
      continue;
    }

    double q = QValue(qnode, sampling);

    if (q > bestq)  // XXX
    {
      bestq = q;
      besta = action;
    }
  }

  assert(besta != -1);
  return besta;
}

double MCTS::HValue(VNODE *vnode, bool sampling) const {
  if (vnode) {  //树上的节点
    return vnode->/*GetCumulativeReward().*/ ThompsonSampling(sampling);  // XXX
  } else if (TreeDepth + 1 >= Params.MaxDepth) {  // search horizon reached
    return 0.0;
  }

  return NormalGammaInfo().ThompsonSampling(sampling);  //按照默认分布返回
}

double MCTS::QValue(QNODE &qnode, bool sampling) const  //改成多层调用？
{
  double qvalue = 0;

  {
    const std::vector<std::pair<int, double>> &observations =
        qnode.GetObservation().ThompsonSampling(sampling);  //得到可能的观察分布
    for (std::vector<std::pair<int, double>>::const_iterator it =
         observations.begin();
         it != observations.end(); ++it) {
      qvalue += it->second * HValue(qnode.Child(it->first), sampling);
    }
  }

  qvalue *= Simulator.GetDiscount();

  {
    const std::vector<std::pair<double, double>> &rewards =
        qnode.GetImmediateReward().ThompsonSampling(
          sampling);  //得到可能的立即收益分布
    for (std::vector<std::pair<double, double>>::const_iterator it =
         rewards.begin();
         it != rewards.end(); ++it) {
      qvalue += it->second * it->first;
    }
  }

  return qvalue;
}

double MCTS::Rollout(STATE &state)  //从 state 出发随机选择动作
{
  if (Params.Verbose >= 3) cout << "Starting rollout" << endl;

  double totalReward = 0.0;
  double discount = 1.0;
  bool terminal = false;
  int numSteps;
  for (numSteps = 0; numSteps + TreeDepth < Params.MaxDepth && !terminal; ++numSteps) {
    int observation;
    double reward;

    int action = Simulator.SelectRandom(state, History);  //根据 knowledge level 随机选择动作
    terminal = Simulator.Step(state, action, observation,
                              reward);  //根据 state 和 action 进行一次模拟
    History.Add(action, observation, Params.MemorySize);

    if (Params.Verbose >= 4) {
      Simulator.DisplayAction(action, cout);
      Simulator.DisplayObservation(state, observation, cout);
      Simulator.DisplayReward(reward, cout);
      Simulator.DisplayState(state, cout);
    }

    totalReward += reward * discount;
    discount *= Simulator.GetDiscount();
  }

  if (Params.Verbose >= 3)
    cout << "Ending rollout after " << numSteps << " steps, with total reward "
         << totalReward << endl;
  return totalReward;
}

void MCTS::ParticleFilter(BELIEF_STATE &beliefs)  // unweighted particle filter
{
  int attempts = 0, added = 0;
  int max_attempts = (Params.NumStartStates - beliefs.GetNumSamples()) * 10;

  int realObs = History.Back().Observation;
  int stepObs;
  double stepReward;

  if (Params.Verbose >= 1) {
    cout << "MCTS::ParticleFilter: last step belief size "
         << Root->Beliefs().GetNumSamples() << ", current belief size "
         << beliefs.GetNumSamples() << endl;
  }

  while (beliefs.GetNumSamples() < Params.NumStartStates &&
         attempts < max_attempts) {
    STATE *state = Root->Beliefs().CreateSample(Simulator);
    Simulator.Step(*state, History.Back().Action, stepObs, stepReward);
    if (Params.ThompsonSampling) {
      Root->Child(History.Back().Action).Update(stepObs, stepReward);
    }

    if (stepObs == realObs) {
      beliefs.AddSample(state);
      added++;
    } else {
      Simulator.FreeState(state);
    }
    attempts++;
  }

  if (Params.Verbose >= 1) {
    cout << "MCTS::ParticleFilter: Created " << added
         << " local transformations out of " << attempts << " attempts" << endl;
  }
}

void MCTS::AddTransforms(BELIEF_STATE &beliefs) {
  int attempts = 0, added = 0;

  if (Params.Verbose >= 1) {
    cout << "MCTS::AddTransforms: last step belief size "
         << Root->Beliefs().GetNumSamples() << ", current belief size "
         << beliefs.GetNumSamples() << endl;
  }

  // Local transformations of state that are consistent with history
  while (added < Params.NumTransforms && attempts < Params.MaxAttempts) {
    STATE *transform = CreateTransform();
    if (transform) {
      beliefs.AddSample(transform);
      added++;
    }
    attempts++;
  }

  if (Params.Verbose >= 1) {
    cout << "MCTS::AddTransforms: Created " << added
         << " local transformations out of " << attempts << " attempts" << endl;
  }
}

STATE *MCTS::CreateTransform() const {
  int stepObs;
  double stepReward;

  STATE *state = Root->Beliefs().CreateSample(Simulator);
  Simulator.Step(*state, History.Back().Action, stepObs, stepReward);
  if (Params.ThompsonSampling) {
    Root->Child(History.Back().Action).Update(stepObs, stepReward);
  }

  if (Simulator.LocalMove(*state, History, stepObs)) return state;

  Simulator.FreeState(state);
  return 0;
}

void MCTS::DisplayValue(int depth, ostream &ostr) const {
  HISTORY history;
  ostr << "MCTS Values:" << endl;

  std::vector<double> qvalues(VNODE::NumChildren);
  for (int action = 0; action < VNODE::NumChildren; action++) {
    QNODE &qnode = Root->Child(action);

    if (qnode.Applicable()) {
      qvalues[action] = QValue(qnode, false);
    }
  }

  Root->DisplayValue(history, depth, ostr, &qvalues);
}

void MCTS::DisplayPolicy(int depth, ostream &ostr) const {
  HISTORY history;
  ostr << "MCTS Policy:" << endl;
  Root->DisplayPolicy(history, depth, ostr);
}

void MCTS::UnitTest() {
  UnitTestGreedy();
  UnitTestUCB();
  UnitTestRollout();
  for (int depth = 1; depth <= 3; ++depth) UnitTestSearch(depth);
}

void MCTS::UnitTestGreedy() {
  TEST_SIMULATOR testSimulator(5, 5, 0);
  PARAMS params;
  MCTS mcts(testSimulator, params);
  int numAct = testSimulator.GetNumActions();

  HISTORY History;
  VNODE* vnode = mcts.ExpandNode(testSimulator.CreateStartState(), History);
  vnode->Value.Set(1, 0);
  vnode->Child(0).Value.Set(1, 1/*, 2, 1*/);
  for (int action = 1; action < numAct; action++)
    vnode->Child(action).Value.Set(0, 0/*, 1, 1*/);

  int besta = mcts.GreedyUCB(vnode, false);
  assert(besta == 0);
}

void MCTS::UnitTestUCB() {
  TEST_SIMULATOR testSimulator(5, 5, 0);
  PARAMS params;
  MCTS mcts(testSimulator, params);
  int numAct = testSimulator.GetNumActions();
  int numObs = testSimulator.GetNumObservations();
  HISTORY History;

  // With equal value, action with lowest count is selected
  VNODE* vnode1 = mcts.ExpandNode(testSimulator.CreateStartState(), History);
  vnode1->Value.Set(1, 0);
  for (int action = 0; action < numAct; action++)
    if (action == 3)
      vnode1->Child(action).Value.Set(99, 0/*, 1, 100*/);
    else
      vnode1->Child(action).Value.Set(100 + action, 0/*, 1, 101 +action*/);
  assert(mcts.GreedyUCB(vnode1, true) == 3);

  // With high counts, action with highest value is selected
  VNODE* vnode2 = mcts.ExpandNode(testSimulator.CreateStartState(), History);
  vnode2->Value.Set(1, 0);
  for (int action = 0; action < numAct; action++)
    if (action == 3)
      vnode2->Child(action).Value.Set(99 + numObs, 1/*, 100 + numObs,1*/);
    else
      vnode2->Child(action).Value.Set(100 + numAct - action, 0/*, 1,101 + numAct - action*/);
  assert(mcts.GreedyUCB(vnode2, true) == 3);

  // Action with low value and low count beats actions with high counts
  VNODE* vnode3 = mcts.ExpandNode(testSimulator.CreateStartState(), History);
  vnode3->Value.Set(1, 0);
  for (int action = 0; action < numAct; action++)
    if (action == 3)
      vnode3->Child(action).Value.Set(1, 1/*, 2, 1*/);
    else
      vnode3->Child(action).Value.Set(100 + action, 1/*, 101 + action,1*/);
  assert(mcts.GreedyUCB(vnode3, true) == 3);

  // Actions with zero count is always selected
  VNODE* vnode4 = mcts.ExpandNode(testSimulator.CreateStartState(), History);
  vnode4->Value.Set(1, 0);
  for (int action = 0; action < numAct; action++)
    if (action == 3)
      vnode4->Child(action).Value.Set(0, 0/*, 1, 1*/);
    else
      vnode4->Child(action).Value.Set(1, 1/*, 2, 1*/);
  assert(mcts.GreedyUCB(vnode4, true) == 3);
}

void MCTS::UnitTestRollout() {
  TEST_SIMULATOR testSimulator(2, 2, 0);
  PARAMS params;
  params.NumSimulations = 1000;
  params.MaxDepth = 10;
  MCTS mcts(testSimulator, params);
  double totalReward = 0.0;
  for (int n = 0; n < mcts.Params.NumSimulations; ++n)
  {
    STATE* state = testSimulator.CreateStartState();
    mcts.TreeDepth = 0;
    totalReward += mcts.Rollout(*state);
  }
  double rootValue = totalReward / mcts.Params.NumSimulations;
  double meanValue = testSimulator.MeanValue();
  assert(fabs(meanValue - rootValue) < 0.1);
}

void MCTS::UnitTestSearch(int depth) {
  TEST_SIMULATOR testSimulator(3, 2, depth);
  PARAMS params;
  params.MaxDepth = depth + 1;
  params.NumSimulations = pow(10, depth + 1);
  MCTS mcts(testSimulator, params);
  mcts.Search();
  double rootValue = mcts.Root->Value.GetValue();
  double optimalValue = testSimulator.OptimalValue();
  assert(fabs(optimalValue - rootValue) < 0.1);
}
