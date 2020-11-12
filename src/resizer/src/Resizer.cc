/////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2019, OpenROAD
// All rights reserved.
//
// BSD 3-Clause License
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived from
//   this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////////

#include "resizer/Resizer.hh"

#include "sta/Report.hh"
#include "sta/Debug.hh"
#include "sta/FuncExpr.hh"
#include "sta/PortDirection.hh"
#include "sta/TimingRole.hh"
#include "sta/Units.hh"
#include "sta/Liberty.hh"
#include "sta/TimingArc.hh"
#include "sta/TimingModel.hh"
#include "sta/Network.hh"
#include "sta/Graph.hh"
#include "sta/DcalcAnalysisPt.hh"
#include "sta/ArcDelayCalc.hh"
#include "sta/GraphDelayCalc.hh"
#include "sta/Parasitics.hh"
#include "sta/Sdc.hh"
#include "sta/Corner.hh"
#include "sta/PathVertex.hh"
#include "sta/SearchPred.hh"
#include "sta/Bfs.hh"
#include "sta/Search.hh"
#include "sta/StaMain.hh"
#include "sta/Fuzzy.hh"
#include "openroad/OpenRoad.hh"
#include "openroad/Error.hh"
#include "resizer/SteinerTree.hh"
#include "opendb/dbTransform.h"

// Outstanding issues
//  multi-corner support?
//  option to place buffers between driver and load on long wires
//   to fix max slew/cap violations
// http://vlsicad.eecs.umich.edu/BK/Slots/cache/dropzone.tamu.edu/~zhuoli/GSRC/fast_buffer_insertion.html

namespace sta {

using std::abs;
using std::min;
using std::max;
using std::string;
using std::to_string;

using ord::warn;
using ord::closestPtInRect;

using odb::dbInst;
using odb::dbPlacementStatus;
using odb::Rect;
using odb::dbOrientType;
using odb::dbTransform;
using odb::dbMPin;
using odb::dbBox;

extern "C" {
extern int Resizer_Init(Tcl_Interp *interp);
}

extern const char *resizer_tcl_inits[];

Resizer::Resizer() :
  StaState(),
  wire_res_(0.0),
  wire_cap_(0.0),
  wire_clk_res_(0.0),
  wire_clk_cap_(0.0),
  corner_(nullptr),
  max_area_(0.0),
  sta_(nullptr),
  db_network_(nullptr),
  db_(nullptr),
  core_exists_(false),
  min_max_(nullptr),
  dcalc_ap_(nullptr),
  pvt_(nullptr),
  parasitics_ap_(nullptr),
  have_estimated_parasitics_(false),
  target_load_map_(nullptr),
  level_drvr_verticies_valid_(false),
  tgt_slews_{0.0, 0.0},
  unique_net_index_(1),
  unique_inst_index_(1),
  resize_count_(0),
  design_area_(0.0)
{
}

void
Resizer::init(Tcl_Interp *interp,
              dbDatabase *db,
              dbSta *sta)
{
  db_ = db;
  block_ = nullptr;
  sta_ = sta;
  db_network_ = sta->getDbNetwork();
  copyState(sta);
  // Define swig TCL commands.
  Resizer_Init(interp);
  // Eval encoded sta TCL sources.
  evalTclInit(interp, resizer_tcl_inits);
}

////////////////////////////////////////////////////////////////

double
Resizer::coreArea() const
{
  return dbuToMeters(core_.dx()) * dbuToMeters(core_.dy());
}

double
Resizer::utilization()
{
  ensureBlock();
  double core_area = coreArea();
  if (core_area > 0.0)
    return design_area_ / core_area;
  else
    return 1.0;
}

double
Resizer::maxArea() const
{
  return max_area_;
}

////////////////////////////////////////////////////////////////

class VertexLevelLess
{
public:
  VertexLevelLess(const Network *network);
  bool operator()(const Vertex *vertex1,
                  const Vertex *vertex2) const;

protected:
  const Network *network_;
};

VertexLevelLess::VertexLevelLess(const Network *network) :
  network_(network)
{
}

bool
VertexLevelLess::operator()(const Vertex *vertex1,
                            const Vertex *vertex2) const
{
  Level level1 = vertex1->level();
  Level level2 = vertex2->level();
  return (level1 < level2)
    || (level1 == level2
        // Break ties for stable results.
        && stringLess(network_->pathName(vertex1->pin()),
                      network_->pathName(vertex2->pin())));
}


////////////////////////////////////////////////////////////////

// block_ indicates core_, design_area_, db_network_ etc valid.
void
Resizer::ensureBlock()
{
  // block_ indicates core_, design_area_
  if (block_ == nullptr) {
    block_ = db_->getChip()->getBlock();
    block_->getCoreArea(core_);
    core_exists_ = !(core_.xMin() == 0
                     && core_.xMax() == 0
                     && core_.yMin() == 0
                     && core_.yMax() == 0);
    design_area_ = findDesignArea();
  }
}

void
Resizer::init()
{
  // Abbreviated copyState
  db_network_ = sta_->getDbNetwork();
  sta_->ensureLevelized();
  graph_ = sta_->graph();
  ensureBlock();
  ensureLevelDrvrVerticies();
  sta_->ensureClkNetwork();
  ensureCorner();
}

void
Resizer::removeBuffers()
{
  ensureBlock();
  db_network_ = sta_->getDbNetwork();
  // Disable incremental timing.
  graph_delay_calc_->delaysInvalid();
  search_->arrivalsInvalid();

  int remove_count = 0;
  for (dbInst *inst : block_->getInsts()) {
    LibertyCell *lib_cell = db_network_->libertyCell(inst);
    if (lib_cell && lib_cell->isBuffer()) {
      LibertyPort *input_port, *output_port;
      lib_cell->bufferPorts(input_port, output_port);
      Instance *buffer = db_network_->dbToSta(inst);
      Pin *input_pin = db_network_->findPin(buffer, input_port);
      Pin *output_pin = db_network_->findPin(buffer, output_port);
      Net *input_net = db_network_->net(input_pin);
      Net *output_net = db_network_->net(output_pin);
      if (!hasTopLevelPort(input_net)
          && !hasTopLevelPort(output_net)) {
        NetPinIterator *pin_iter = db_network_->pinIterator(output_net);
        while (pin_iter->hasNext()) {
          Pin *pin = pin_iter->next();
          if (pin != output_pin) {
            Instance *pin_inst = db_network_->instance(pin);
            Port *pin_port = db_network_->port(pin);
            sta_->disconnectPin(pin);
            sta_->connectPin(pin_inst, pin_port, input_net);
          }
        }
        delete pin_iter;
        sta_->deleteNet(output_net);
        sta_->deleteInstance(buffer);
        remove_count++;
      }
    }
  }
  printf("Removed %d buffers.\n", remove_count);
}

void
Resizer::setWireRC(float wire_res,
                   float wire_cap,
                   Corner *corner)
{
  setWireCorner(corner);
  wire_res_ = wire_res;
  wire_cap_ = wire_cap;
}

void
Resizer::setWireClkRC(float wire_res,
                      float wire_cap,
                      Corner *corner)
{
  setWireCorner(corner);
  wire_clk_res_ = wire_res;
  wire_clk_cap_ = wire_cap;
}

void
Resizer::setWireCorner(Corner *corner)
{
  initCorner(corner);
  // Abbreviated copyState
  graph_delay_calc_ = sta_->graphDelayCalc();
  search_ = sta_->search();
  graph_ = sta_->ensureGraph();

  sta_->ensureLevelized();
  // Disable incremental timing.
  graph_delay_calc_->delaysInvalid();
  search_->arrivalsInvalid();
}

void
Resizer::ensureCorner()
{
  if (corner_ == nullptr)
    initCorner(sta_->cmdCorner());
}

void
Resizer::initCorner(Corner *corner)
{
  corner_ = corner;
  min_max_ = MinMax::max();
  dcalc_ap_ = corner->findDcalcAnalysisPt(min_max_);
  pvt_ = dcalc_ap_->operatingConditions();
  parasitics_ap_ = corner->findParasiticAnalysisPt(min_max_);
}

void
Resizer::ensureLevelDrvrVerticies()
{
  if (!level_drvr_verticies_valid_) {
    level_drvr_verticies_.clear();
    VertexIterator vertex_iter(graph_);
    while (vertex_iter.hasNext()) {
      Vertex *vertex = vertex_iter.next();
      if (vertex->isDriver(network_))
        level_drvr_verticies_.push_back(vertex);
    }
    sort(level_drvr_verticies_, VertexLevelLess(network_));
    level_drvr_verticies_valid_ = true;
  }
}

////////////////////////////////////////////////////////////////

void
Resizer::resizePreamble(LibertyLibrarySeq *resize_libs)
{
  init();
  makeEquivCells(resize_libs);
  findTargetLoads(resize_libs);
}

////////////////////////////////////////////////////////////////

void
Resizer::bufferInputs(LibertyCell *buffer_cell)
{
  init();
  inserted_buffer_count_ = 0;
  InstancePinIterator *port_iter = network_->pinIterator(network_->topInstance());
  while (port_iter->hasNext()) {
    Pin *pin = port_iter->next();
    Net *net = network_->net(network_->term(pin));
    if (network_->direction(pin)->isInput()
        && !sta_->isClock(pin)
        && !isSpecial(net))
      bufferInput(pin, buffer_cell);
  }
  delete port_iter;
  if (inserted_buffer_count_ > 0) {
    printf("Inserted %d input buffers.\n", inserted_buffer_count_);
    level_drvr_verticies_valid_ = false;
  }
}
   
void
Resizer::bufferInput(Pin *top_pin,
                     LibertyCell *buffer_cell)
{
  Term *term = db_network_->term(top_pin);
  Net *input_net = db_network_->net(term);
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);
  string buffer_out_name = makeUniqueNetName();
  string buffer_name = makeUniqueInstName("input");
  Instance *parent = db_network_->topInstance();
  Net *buffer_out = db_network_->makeNet(buffer_out_name.c_str(), parent);
  Instance *buffer = db_network_->makeInstance(buffer_cell,
                                               buffer_name.c_str(),
                                               parent);
  if (buffer) {
    Point pin_loc = db_network_->location(top_pin);
    Point buf_loc = closestPtInRect(core_, pin_loc);
    setLocation(buffer, buf_loc);
    design_area_ += area(db_network_->cell(buffer_cell));
    inserted_buffer_count_++;

    NetPinIterator *pin_iter = db_network_->pinIterator(input_net);
    while (pin_iter->hasNext()) {
      Pin *pin = pin_iter->next();
      // Leave input port pin connected to input_net.
      if (pin != top_pin) {
        sta_->disconnectPin(pin);
        Port *pin_port = db_network_->port(pin);
        sta_->connectPin(db_network_->instance(pin), pin_port, buffer_out);
      }
    }
    delete pin_iter;
    sta_->connectPin(buffer, input, input_net);
    sta_->connectPin(buffer, output, buffer_out);
  }
}

void
Resizer::setLocation(Instance *inst,
                     Point pt)
{
  dbInst *dinst = db_network_->staToDb(inst);
  dinst->setPlacementStatus(dbPlacementStatus::PLACED);
  dinst->setLocation(pt.getX(), pt.getY());
}

void
Resizer::bufferOutputs(LibertyCell *buffer_cell)
{
  init();
  inserted_buffer_count_ = 0;
  InstancePinIterator *port_iter = network_->pinIterator(network_->topInstance());
  while (port_iter->hasNext()) {
    Pin *pin = port_iter->next();
    Net *net = network_->net(network_->term(pin));
    if (network_->direction(pin)->isOutput()
        && net
        && !isSpecial(net))
      bufferOutput(pin, buffer_cell);
  }
  delete port_iter;
  if (inserted_buffer_count_ > 0) {
    printf("Inserted %d output buffers.\n", inserted_buffer_count_);
    level_drvr_verticies_valid_ = false;
  }
}

void
Resizer::bufferOutput(Pin *top_pin,
                     LibertyCell *buffer_cell)
{
  NetworkEdit *network = networkEdit();
  Term *term = network_->term(top_pin);
  Net *output_net = network_->net(term);
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);
  string buffer_in_net_name = makeUniqueNetName();
  string buffer_name = makeUniqueInstName("output");
  Instance *parent = network->topInstance();
  Net *buffer_in = network->makeNet(buffer_in_net_name.c_str(), parent);
  Instance *buffer = network->makeInstance(buffer_cell,
                                           buffer_name.c_str(),
                                           parent);
  if (buffer) {
    setLocation(buffer, db_network_->location(top_pin));
    design_area_ += area(db_network_->cell(buffer_cell));
    inserted_buffer_count_++;

    NetPinIterator *pin_iter = network->pinIterator(output_net);
    while (pin_iter->hasNext()) {
      Pin *pin = pin_iter->next();
      if (pin != top_pin) {
        // Leave output port pin connected to output_net.
        sta_->disconnectPin(pin);
        Port *pin_port = network->port(pin);
        sta_->connectPin(network->instance(pin), pin_port, buffer_in);
      }
    }
    delete pin_iter;
    sta_->connectPin(buffer, input, buffer_in);
    sta_->connectPin(buffer, output, output_net);
  }
}

////////////////////////////////////////////////////////////////

void
Resizer::resizeToTargetSlew()
{
  resize_count_ = 0;
  resized_multi_output_insts_.clear();
  // Resize in reverse level order.
  for (int i = level_drvr_verticies_.size() - 1; i >= 0; i--) {
    Vertex *drvr = level_drvr_verticies_[i];
    Pin *drvr_pin = drvr->pin();
    Net *net = network_->net(drvr_pin);
    Instance *inst = network_->instance(drvr_pin);
    if (net
        && !drvr->isConstant()
        && hasFanout(drvr)
        // Hands off the clock nets.
        && !sta_->isClock(drvr_pin)
        // Hands off special nets.
        && !isSpecial(net)) {
      resizeToTargetSlew(drvr_pin);
      if (overMaxArea()) {
        warn("Max utilization reached.");
        break;
      }
    }
  }
  ensureWireParasitics();
  printf("Resized %d instances.\n", resize_count_);
}

bool
Resizer::hasFanout(Vertex *drvr)
{
  VertexOutEdgeIterator edge_iter(drvr, graph_);
  return edge_iter.hasNext();
}

void
Resizer::makeEquivCells(LibertyLibrarySeq *resize_libs)
{
  // Map cells from all libraries to resize_libs.
  LibertyLibrarySeq map_libs;
  LibertyLibraryIterator *lib_iter = network_->libertyLibraryIterator();
  while (lib_iter->hasNext()) {
    LibertyLibrary *lib = lib_iter->next();
    map_libs.push_back(lib);
  }
  delete lib_iter;
  sta_->makeEquivCells(resize_libs, &map_libs);
}

void
Resizer::resizeToTargetSlew(const Pin *drvr_pin)
{
  NetworkEdit *network = networkEdit();
  Instance *inst = network_->instance(drvr_pin);
  LibertyCell *cell = network_->libertyCell(inst);
  if (cell) {
    LibertyCellSeq *equiv_cells = sta_->equivCells(cell);
    if (equiv_cells) {
      bool revisiting_inst = false;
      if (hasMultipleOutputs(inst)) {
        if (resized_multi_output_insts_.hasKey(inst))
          revisiting_inst = true;
        debugPrint1(debug_, "resizer", 2, "multiple outputs%s\n",
                    revisiting_inst ? " - revisit" : "");
        resized_multi_output_insts_.insert(inst);
      }
      bool is_buf_inv = cell->isBuffer() || cell->isInverter();
      ensureWireParasitic(drvr_pin);
      // Includes net parasitic capacitance.
      float load_cap = graph_delay_calc_->loadCap(drvr_pin, dcalc_ap_);
      if (load_cap > 0.0) {
        LibertyCell *best_cell = cell;
        float target_load = (*target_load_map_)[cell];
        float best_load = target_load;
        float best_ratio = (target_load < load_cap)
          ? target_load / load_cap
          : load_cap / target_load;
        float best_delay = is_buf_inv ? bufferDelay(cell, load_cap) : 0.0;
        debugPrint4(debug_, "resizer", 2, "%s load cap %s ratio=%.2f delay=%s\n",
                    sdc_network_->pathName(drvr_pin),
                    units_->capacitanceUnit()->asString(load_cap),
                    best_ratio,
                    units_->timeUnit()->asString(best_delay, 3));
        for (LibertyCell *target_cell : *equiv_cells) {
          if (!dontUse(target_cell)) {
            float target_load = (*target_load_map_)[target_cell];
            float delay = is_buf_inv ? bufferDelay(target_cell, load_cap) : 0.0;
            float ratio = target_load / load_cap;
            if (ratio > 1.0)
              ratio = 1.0 / ratio;
            debugPrint3(debug_, "resizer", 2, " %s ratio=%.2f delay=%s\n",
                        target_cell->name(),
                        ratio,
                        units_->timeUnit()->asString(delay, 3));
            if (is_buf_inv
                // Library may have "delay" buffers/inverters that are
                // functionally buffers/inverters but have additional
                // intrinsic delay. Accept worse target load matching if
                // delay is reduced to avoid using them.
                ? ((delay < best_delay
                    && ratio > best_ratio * 0.9)
                   || (ratio > best_ratio
                       && delay < best_delay * 1.1))
                : ratio > best_ratio
                // If the instance has multiple outputs (generally a register Q/QN)
                // only allow upsizing after the first pin is visited.
                && (!revisiting_inst
                    || target_load > best_load)) {
              best_cell = target_cell;
              best_ratio = ratio;
              best_load = target_load;
              best_delay = delay;
            }
          }
        }
        if (best_cell != cell) {
          debugPrint3(debug_, "resizer", 2, "%s %s -> %s\n",
                      sdc_network_->pathName(drvr_pin),
                      cell->name(),
                      best_cell->name());
          const char *best_cell_name = best_cell->name();
          dbMaster *best_master = db_->findMaster(best_cell_name);
          // Replace LEF with LEF so ports stay aligned in instance.
          if (best_master) {
            dbInst *dinst = db_network_->staToDb(inst);
            dbMaster *master = dinst->getMaster();
            design_area_ -= area(master);
            Cell *best_cell1 = db_network_->dbToSta(best_master);
            sta_->replaceCell(inst, best_cell1);
            if (!revisiting_inst)
              resize_count_++;
            design_area_ += area(best_master);

            // Delete estimated parasitics on all instance pins.
            // Input nets change pin cap, outputs change location (slightly).
            if (have_estimated_parasitics_) {
              InstancePinIterator *pin_iter = network_->pinIterator(inst);
              while (pin_iter->hasNext()) {
                const Pin *pin = pin_iter->next();
                const Net *net = network_->net(pin);
                if (net) {
                  debugPrint1(debug_, "resizer_parasitics", 1, "delete parasitic %s\n",
                              network_->pathName(net));
                  parasitics_->deleteParasitics(net, parasitics_ap_);
                }
              }
              delete pin_iter;
            }
          }
        }
      }
    }
  }
}

bool
Resizer::hasMultipleOutputs(const Instance *inst)
{
  int output_count = 0;
  InstancePinIterator *pin_iter = network_->pinIterator(inst);
  while (pin_iter->hasNext()) {
    const Pin *pin = pin_iter->next();
    if (network_->direction(pin)->isAnyOutput()
        && network_->net(pin)) {
      output_count++;
      if (output_count > 1)
        return true;
    }
  }
  return false;
}

void
Resizer::ensureWireParasitic(const Pin *drvr_pin)
{
  if (have_estimated_parasitics_
      && parasitics_->findPiElmore(drvr_pin, RiseFall::rise(),
                                   parasitics_ap_) == nullptr) {
    const Net *net = network_->net(drvr_pin);
    if (net)
      estimateWireParasitic(net);
  }
}

double
Resizer::area(Cell *cell)
{
  return area(db_network_->staToDb(cell));
}

double
Resizer::area(dbMaster *master)
{
  if (!master->isCoreAutoPlaceable()) {
    return 0;
  }
  return dbuToMeters(master->getWidth()) * dbuToMeters(master->getHeight());
}

double
Resizer::dbuToMeters(int dist) const
{
  int dbu = db_->getTech()->getDbUnitsPerMicron();
  return dist / (dbu * 1e+6);
}

int
Resizer::metersToDbu(double dist) const
{
  int dbu = db_->getTech()->getDbUnitsPerMicron();
  return dist * dbu * 1e+6;
}

void
Resizer::setMaxUtilization(double max_utilization)
{
  max_area_ = coreArea() * max_utilization;
}

bool
Resizer::overMaxArea()
{
  return max_area_
    && fuzzyGreaterEqual(design_area_, max_area_);
}

void
Resizer::setDontUse(LibertyCellSeq *dont_use)
{
  if (dont_use) {
    for (LibertyCell *cell : *dont_use)
      dont_use_.insert(cell);
  }
}

bool
Resizer::dontUse(LibertyCell *cell)
{
  return cell->dontUse()
    || dont_use_.hasKey(cell);
}

////////////////////////////////////////////////////////////////

// Find a target slew for the libraries and then
// a target load for each cell that gives the target slew.
void
Resizer::findTargetLoads(LibertyLibrarySeq *resize_libs)
{
  // Find target slew across all buffers in the libraries.
  findBufferTargetSlews(resize_libs);
  if (target_load_map_ == nullptr)
    target_load_map_ = new CellTargetLoadMap;
  target_load_map_->clear();
  for (LibertyLibrary *lib : *resize_libs)
    findTargetLoads(lib, tgt_slews_);
}

float
Resizer::targetLoadCap(LibertyCell *cell)
{
  float load_cap = 0.0;
  bool exists;
  target_load_map_->findKey(cell, load_cap, exists);
  return load_cap;
}

void
Resizer::findTargetLoads(LibertyLibrary *library,
                         TgtSlews &slews)
{
  LibertyCellIterator cell_iter(library);
  while (cell_iter.hasNext()) {
    LibertyCell *cell = cell_iter.next();
    findTargetLoad(cell, slews);
  }
}

void
Resizer::findTargetLoad(LibertyCell *cell,
                        TgtSlews &slews)
{
  LibertyCellTimingArcSetIterator arc_set_iter(cell);
  float target_load_sum[RiseFall::index_count]{0.0};
  int arc_count[RiseFall::index_count]{0};

  while (arc_set_iter.hasNext()) {
    TimingArcSet *arc_set = arc_set_iter.next();
    TimingRole *role = arc_set->role();
    if (!role->isTimingCheck()
        && role != TimingRole::tristateDisable()
        && role != TimingRole::tristateEnable()) {
      TimingArcSetArcIterator arc_iter(arc_set);
      while (arc_iter.hasNext()) {
        TimingArc *arc = arc_iter.next();
        int in_rf_index = arc->fromTrans()->asRiseFall()->index();
        int out_rf_index = arc->toTrans()->asRiseFall()->index();
        float arc_target_load = findTargetLoad(cell, arc,
                                               slews[in_rf_index],
                                               slews[out_rf_index]);
        target_load_sum[out_rf_index] += arc_target_load;
        arc_count[out_rf_index]++;
      }
    }
  }
  float target_load = INF;
  for (int rf : RiseFall::rangeIndex()) {
    if (arc_count[rf] > 0) {
      float target = target_load_sum[rf] / arc_count[rf];
      target_load = min(target_load, target);
    }
  }
  (*target_load_map_)[cell] = target_load;
  debugPrint2(debug_, "resizer", 3, "%s target_load = %.2e\n",
              cell->name(),
              target_load);
}

// Find the load capacitance that will cause the output slew
// to be equal to out_slew.
float
Resizer::findTargetLoad(LibertyCell *cell,
                        TimingArc *arc,
                        Slew in_slew,
                        Slew out_slew)
{
  GateTimingModel *model = dynamic_cast<GateTimingModel*>(arc->model());
  if (model) {
    float cap_init = 1.0e-12;  // 1pF
    float cap_tol = 0.1e-15; // .1fF
    float load_cap = cap_init;
    float cap_step = cap_init;
    Slew prev_slew = 0.0;
    while (cap_step > cap_tol) {
      ArcDelay arc_delay;
      Slew arc_slew;
      model->gateDelay(cell, pvt_, in_slew, load_cap, 0.0, false,
                       arc_delay, arc_slew);
      if (arc_slew > out_slew) {
        load_cap -= cap_step;
        cap_step /= 2.0;
      }
      load_cap += cap_step;
      if (arc_slew == prev_slew)
        // we are stuck
        break;
      prev_slew = arc_slew;
    }
    return load_cap;
  }
  return 0.0;
}

////////////////////////////////////////////////////////////////

Slew
Resizer::targetSlew(const RiseFall *rf)
{
  return tgt_slews_[rf->index()];
}

// Find target slew across all buffers in the libraries.
void
Resizer::findBufferTargetSlews(LibertyLibrarySeq *resize_libs)
{
  tgt_slews_[RiseFall::riseIndex()] = 0.0;
  tgt_slews_[RiseFall::fallIndex()] = 0.0;
  int tgt_counts[RiseFall::index_count]{0};
  
  for (LibertyLibrary *lib : *resize_libs) {
    Slew slews[RiseFall::index_count]{0.0};
    int counts[RiseFall::index_count]{0};
    
    findBufferTargetSlews(lib, slews, counts);
    for (int rf : RiseFall::rangeIndex()) {
      tgt_slews_[rf] += slews[rf];
      tgt_counts[rf] += counts[rf];
      slews[rf] /= counts[rf];
    }
    debugPrint3(debug_, "resizer", 2, "target_slews %s = %s/%s\n",
                lib->name(),
                units_->timeUnit()->asString(slews[RiseFall::riseIndex()], 3),
                units_->timeUnit()->asString(slews[RiseFall::fallIndex()], 3));
  }

  for (int rf : RiseFall::rangeIndex())
    tgt_slews_[rf] /= tgt_counts[rf];

  debugPrint2(debug_, "resizer", 1, "target_slews = %s/%s\n",
              units_->timeUnit()->asString(tgt_slews_[RiseFall::riseIndex()], 3),
              units_->timeUnit()->asString(tgt_slews_[RiseFall::fallIndex()], 3));

//    printf("Target slews rise %s / fall %s\n",
//           units_->timeUnit()->asString(tgt_slews_[RiseFall::riseIndex()], 3),
//           units_->timeUnit()->asString(tgt_slews_[RiseFall::fallIndex()], 3));
}

void
Resizer::findBufferTargetSlews(LibertyLibrary *library,
                               // Return values.
                               Slew slews[],
                               int counts[])
{
  for (LibertyCell *buffer : *library->buffers()) {
    if (!dontUse(buffer)) {
      LibertyPort *input, *output;
      buffer->bufferPorts(input, output);
      TimingArcSetSeq *arc_sets = buffer->timingArcSets(input, output);
      if (arc_sets) {
        for (TimingArcSet *arc_set : *arc_sets) {
          TimingArcSetArcIterator arc_iter(arc_set);
          while (arc_iter.hasNext()) {
            TimingArc *arc = arc_iter.next();
            GateTimingModel *model = dynamic_cast<GateTimingModel*>(arc->model());
            RiseFall *in_rf = arc->fromTrans()->asRiseFall();
            RiseFall *out_rf = arc->toTrans()->asRiseFall();
            float in_cap = input->capacitance(in_rf, min_max_);
            float load_cap = in_cap * 10.0; // "factor debatable"
            ArcDelay arc_delay;
            Slew arc_slew;
            model->gateDelay(buffer, pvt_, 0.0, load_cap, 0.0, false,
                             arc_delay, arc_slew);
            model->gateDelay(buffer, pvt_, arc_slew, load_cap, 0.0, false,
                             arc_delay, arc_slew);
            slews[out_rf->index()] += arc_slew;
            counts[out_rf->index()]++;
          }
        }
      }
    }
  }
}

////////////////////////////////////////////////////////////////

void
Resizer::estimateWireParasitics()
{
  if (wire_cap_ > 0.0) {
    sta_->ensureClkNetwork();

    sta_->deleteParasitics();
    corner_ = sta_->cmdCorner();
    sta_->corners()->makeParasiticAnalysisPtsSingle();
    parasitics_ap_ = corner_->findParasiticAnalysisPt(MinMax::max());

    NetIterator *net_iter = network_->netIterator(network_->topInstance());
    while (net_iter->hasNext()) {
      Net *net = net_iter->next();
      // Estimate parastices for clocks also for when they are propagated.
      if (!network_->isPower(net)
          && !network_->isGround(net))
        estimateWireParasitic(net);
    }
    delete net_iter;
    have_estimated_parasitics_ = true;
  }
}

void
Resizer::estimateWireParasitic(const dbNet *net)
{
  estimateWireParasitic(db_network_->dbToSta(net));
}
 
void
Resizer::estimateWireParasitic(const Net *net)
{
  // Do not add parasitics on ports.
  // When the input drives a pad instance with huge input
  // cap the elmore delay is gigantic.
  if (!hasTopLevelPort(net)) {
    SteinerTree *tree = makeSteinerTree(net, false, db_network_);
    if (tree) {
      debugPrint1(debug_, "resizer_parasitics", 1, "estimate wire %s\n",
                  sdc_network_->pathName(net));
      Parasitic *parasitic = parasitics_->makeParasiticNetwork(net, false,
                                                               parasitics_ap_);
      bool is_clk = !sta_->isClock(net);
      int branch_count = tree->branchCount();
      for (int i = 0; i < branch_count; i++) {
        Point pt1, pt2;
        Pin *pin1, *pin2;
        SteinerPt steiner_pt1, steiner_pt2;
        int wire_length_dbu;
        tree->branch(i,
                     pt1, pin1, steiner_pt1,
                     pt2, pin2, steiner_pt2,
                     wire_length_dbu);
        ParasiticNode *n1 = findParasiticNode(tree, parasitic, net, pin1, steiner_pt1);
        ParasiticNode *n2 = findParasiticNode(tree, parasitic, net, pin2, steiner_pt2);
        if (n1 != n2) {
          if (wire_length_dbu == 0)
            // Use a small resistor to keep the connectivity intact.
            parasitics_->makeResistor(nullptr, n1, n2, 1.0e-3, parasitics_ap_);
          else {
            float wire_length = dbuToMeters(wire_length_dbu);
            float wire_cap = wire_length * (is_clk ? wire_clk_cap_ : wire_cap_);
            float wire_res = wire_length * (is_clk ? wire_clk_res_ : wire_res_);
            // Make pi model for the wire.
            debugPrint5(debug_, "resizer_parasitics", 2,
                        " pi %s c2=%s rpi=%s c1=%s %s\n",
                        parasitics_->name(n1),
                        units_->capacitanceUnit()->asString(wire_cap / 2.0),
                        units_->resistanceUnit()->asString(wire_res),
                        units_->capacitanceUnit()->asString(wire_cap / 2.0),
                        parasitics_->name(n2));
            parasitics_->incrCap(n1, wire_cap / 2.0, parasitics_ap_);
            parasitics_->makeResistor(nullptr, n1, n2, wire_res, parasitics_ap_);
            parasitics_->incrCap(n2, wire_cap / 2.0, parasitics_ap_);
          }
        }
      }
      ReduceParasiticsTo reduce_to = ReduceParasiticsTo::pi_elmore;
      const OperatingConditions *op_cond = sdc_->operatingConditions(MinMax::max());
      parasitics_->reduceTo(parasitic, net, reduce_to, op_cond,
                            corner_, MinMax::max(), parasitics_ap_);
      parasitics_->deleteParasiticNetwork(net, parasitics_ap_);
      delete tree;
    }
  }
}

ParasiticNode *
Resizer::findParasiticNode(SteinerTree *tree,
                           Parasitic *parasitic,
                           const Net *net,
                           const Pin *pin,
                           SteinerPt steiner_pt)
{
  if (pin == nullptr)
    // If the steiner pt is on top of a pin, use the pin instead.
    pin = tree->steinerPtAlias(steiner_pt);
  if (pin)
    return parasitics_->ensureParasiticNode(parasitic, pin);
  else 
    return parasitics_->ensureParasiticNode(parasitic, net, steiner_pt);
}

bool
Resizer::hasTopLevelPort(const Net *net)
{
  bool has_top_level_port = false;
  NetConnectedPinIterator *pin_iter = network_->connectedPinIterator(net);
  while (pin_iter->hasNext()) {
    Pin *pin = pin_iter->next();
    if (network_->isTopLevelPort(pin)) {
      has_top_level_port = true;
      break;
    }
  }
  delete pin_iter;
  return has_top_level_port;
}

void
Resizer::ensureWireParasitics()
{
  if (have_estimated_parasitics_) {
    NetIterator *net_iter = network_->netIterator(network_->topInstance());
    while (net_iter->hasNext()) {
      Net *net = net_iter->next();
      // Estimate parastices for clocks also for when they are propagated.
      if (!network_->isPower(net)
          && !network_->isGround(net)) {
        PinSet *drivers = network_->drivers(net);
        if (drivers && !drivers->empty()) {
          PinSet::Iterator drvr_iter(drivers);
          Pin *drvr_pin = drvr_iter.next();
          ensureWireParasitic(drvr_pin);
        }
      }
    }
    delete net_iter;
  }
}

////////////////////////////////////////////////////////////////

// Repair tie hi/low net driver fanout by duplicating the
// tie hi/low instances for every pin connected to tie hi/low instances.
void
Resizer::repairTieFanout(LibertyPort *tie_port,
                         double separation, // meters
                         bool verbose)
{
  ensureBlock();
  Instance *top_inst = network_->topInstance();
  LibertyCell *tie_cell = tie_port->libertyCell();
  InstanceSeq insts;
  findCellInstances(tie_cell, insts);
  int tie_count = 0;
  Instance *parent = db_network_->topInstance();
  int separation_dbu = metersToDbu(separation);
  for (Instance *inst : insts) {
    Pin *drvr_pin = network_->findPin(inst, tie_port);
    if (drvr_pin) {
      const char *inst_name = network_->name(inst);
      Net *net = network_->net(drvr_pin);
      if (net) {
        NetConnectedPinIterator *pin_iter = network_->connectedPinIterator(net);
        while (pin_iter->hasNext()) {
          Pin *load = pin_iter->next();
          if (load != drvr_pin) {
            // Make tie inst.
            Point tie_loc = tieLocation(load, separation_dbu);
            Instance *load_inst = network_->instance(load);
            string tie_name = makeUniqueInstName(inst_name, true);
            Instance *tie = sta_->makeInstance(tie_name.c_str(),
                                               tie_cell, top_inst);
            setLocation(tie, tie_loc);

            // Make tie output net.
            string load_net_name = makeUniqueNetName();
            Net *load_net = db_network_->makeNet(load_net_name.c_str(), top_inst);

            // Connect tie inst output.
            sta_->connectPin(tie, tie_port, load_net);

            // Connect load to tie output net.
            sta_->disconnectPin(load);
            Port *load_port = network_->port(load);
            sta_->connectPin(load_inst, load_port, load_net);

            design_area_ += area(db_network_->cell(tie_cell));
            tie_count++;
          }
        }
        delete pin_iter;

        // Delete inst output net.
        Pin *tie_pin = network_->findPin(inst, tie_port);
        Net *tie_net = network_->net(tie_pin);
        sta_->deleteNet(tie_net);
        // Delete the tie instance.
        sta_->deleteInstance(inst);
      }
    }
  }

  if (tie_count > 0) {
    printf("Inserted %d tie %s instances.\n",
           tie_count,
           tie_cell->name());
    level_drvr_verticies_valid_ = false;
  }
}

void
Resizer::findCellInstances(LibertyCell *cell,
                           // Return value.
                           InstanceSeq &insts)
{
  LeafInstanceIterator *inst_iter = network_->leafInstanceIterator();
  while (inst_iter->hasNext()) {
    Instance *inst = inst_iter->next();
    if (network_->libertyCell(inst) == cell)
      insts.push_back(inst);
  }
  delete inst_iter;
}

Point
Resizer::tieLocation(Pin *load,
                     int separation)
{
  Point load_loc = db_network_->location(load);
  int load_x = load_loc.getX();
  int load_y = load_loc.getY();
  int tie_x = load_x;
  int tie_y = load_y;
  if (!network_->isTopLevelPort(load)) {
    dbInst *db_inst = db_network_->staToDb(network_->instance(load));
    dbBox *bbox = db_inst->getBBox();
    int left_dist = abs(load_x - bbox->xMin());
    int right_dist = abs(load_x - bbox->xMax());
    int bot_dist = abs(load_y - bbox->yMin());
    int top_dist = abs(load_y - bbox->yMax());
    if (left_dist < right_dist
        && left_dist < bot_dist
        && left_dist < top_dist)
      // left
      tie_x -= separation;
    if (right_dist < left_dist
        && right_dist < bot_dist
        && right_dist < top_dist)
      // right
      tie_x += separation;
    if (bot_dist < left_dist
        && bot_dist < right_dist
        && bot_dist < top_dist)
      // bot
      tie_y -= separation;
    if (top_dist < left_dist
        && top_dist < right_dist
        && top_dist < bot_dist)
      // top
      tie_y += separation;
  }
  if (core_exists_)
    return closestPtInRect(core_, tie_x, tie_y);
  else
    return Point(tie_x, tie_y);
}

////////////////////////////////////////////////////////////////

void
Resizer::repairHoldViolations(LibertyCellSeq *buffers,
                              bool allow_setup_violations)
{
  init();
  sta_->findRequireds();
  Search *search = sta_->search();
  VertexSet *ends = sta_->search()->endpoints();
  LibertyCell *buffer_cell = (*buffers)[0];
  repairHoldViolations(ends, buffer_cell, allow_setup_violations);
}

// For testing/debug.
void
Resizer::repairHoldViolations(Pin *end_pin,
                              LibertyCellSeq *buffers,
                              bool allow_setup_violations)
{
  Vertex *end = graph_->pinLoadVertex(end_pin);
  VertexSet ends;
  ends.insert(end);

  init();
  sta_->findRequireds();
  LibertyCell *buffer_cell = (*buffers)[0];
  repairHoldViolations(&ends, buffer_cell, allow_setup_violations);
}

void
Resizer::repairHoldViolations(VertexSet *ends,
                              LibertyCell *buffer_cell,
                              bool allow_setup_violations)
{
  // Find endpoints with hold violation.
  VertexSet hold_failures;
  Slack worst_slack;
  findHoldViolations(ends, worst_slack, hold_failures);
  if (!hold_failures.empty()) {
    printf("Found %lu endpoints with hold violations.\n",
           hold_failures.size());
    inserted_buffer_count_ = 0;
    int repair_count = 1;
    int pass = 1;
    float buffer_delay = bufferDelay(buffer_cell);
    while (!hold_failures.empty()
           // Make sure we are making progress.
           && repair_count > 0) {
      repair_count = repairHoldPass(hold_failures, buffer_cell, buffer_delay,
                                    allow_setup_violations);
      debugPrint4(debug_, "repair_hold", 1,
                  "pass %d worst slack %s failures %lu inserted %d\n",
                  pass,
                  units_->timeUnit()->asString(worst_slack, 3),
                  hold_failures .size(),
                  repair_count);
      sta_->findRequireds();
      findHoldViolations(ends, worst_slack, hold_failures);
      pass++;
    }
    if (inserted_buffer_count_ > 0) {
      printf("Inserted %d hold buffers.\n", inserted_buffer_count_);
      level_drvr_verticies_valid_ = false;
    }
  }
  else
    printf("No hold violations found.\n");
}

void
Resizer::findHoldViolations(VertexSet *ends,
                            // Return values.
                            Slack &worst_slack,
                            VertexSet &hold_violations)
{
  Search *search = sta_->search();
  worst_slack = INF;
  hold_violations.clear();
  debugPrint0(debug_, "repair_hold", 3, "Hold violations\n");
  for (Vertex *end : *ends) {
    Slack slack = sta_->vertexSlack(end, MinMax::min());
    if (!sta_->isClock(end->pin())
        && fuzzyLess(slack, 0.0)) {
      debugPrint1(debug_, "repair_hold", 3, " %s\n",
                  end->name(sdc_network_));
      if (slack < worst_slack)
        worst_slack = slack;
      hold_violations.insert(end);
    }
  }
}

int
Resizer::repairHoldPass(VertexSet &hold_failures,
                        LibertyCell *buffer_cell,
                        float buffer_delay,
                        bool allow_setup_violations)
{
  VertexSet fanins = findHoldFanins(hold_failures);
  VertexSeq sorted_fanins = sortHoldFanins(fanins);
  
  int repair_count = 0;
  int max_repair_count = max(static_cast<int>(hold_failures.size() * .2), 10);
  for(int i = 0; i < sorted_fanins.size() && repair_count < max_repair_count ; i++) {
    Vertex *vertex = sorted_fanins[i];
    Pin *drvr_pin = vertex->pin();
    Net *net = network_->isTopLevelPort(drvr_pin)
      ? network_->net(network_->term(drvr_pin))
      : network_->net(drvr_pin);
    Slack hold_slack = sta_->vertexSlack(vertex, MinMax::min());
    if (hold_slack < 0
        // Hands off special nets.
        && !isSpecial(net)) {
      // Only add delay to loads with hold violations.
      PinSeq load_pins;
      Slack buffer_delay = INF;
      VertexOutEdgeIterator edge_iter(vertex, graph_);
      while (edge_iter.hasNext()) {
        Edge *edge = edge_iter.next();
        Vertex *fanout = edge->to(graph_);
        Slacks slacks;
        sta_->vertexSlacks(fanout, slacks);
        Slack hold_slack = holdSlack(slacks);
        if (hold_slack < 0.0) {
          Delay delay = allow_setup_violations
            ? -hold_slack
            : min(-hold_slack, setupSlack(slacks));
          if (delay > 0.0) {
            buffer_delay = min(buffer_delay, delay);
            load_pins.push_back(fanout->pin());
          }
        }
      }
      if (!load_pins.empty()) {
        int buffer_count = std::ceil(buffer_delay / buffer_delay);
        debugPrint5(debug_, "repair_hold", 2,
                    " %s hold=%s inserted %d for %lu/%d loads\n",
                    vertex->name(sdc_network_),
                    delayAsString(hold_slack, this),
                    buffer_count,
                    load_pins.size(),
                    fanout(vertex));
        makeHoldDelay(vertex, buffer_count, load_pins, buffer_cell);
        repair_count += buffer_count;
        if (overMaxArea()) {
          warn("max utilization reached.");
          return repair_count;
        }
      }
    }
  }
  return repair_count;
}

VertexSet
Resizer::findHoldFanins(VertexSet &ends)
{
  Search *search = sta_->search();
  SearchPredNonReg2 pred(sta_);
  BfsBkwdIterator iter(BfsIndex::other, &pred, this);
  for (Vertex *vertex : ends)
    iter.enqueueAdjacentVertices(vertex);

  VertexSet fanins;
  while (iter.hasNext()) {
    Vertex *fanin = iter.next();
    if (!sta_->isClock(fanin->pin())) {
      if (fanin->isDriver(network_))
        fanins.insert(fanin);
      iter.enqueueAdjacentVertices(fanin);
    }
  }
  return fanins;
}

VertexSeq
Resizer::sortHoldFanins(VertexSet &fanins)
{
  VertexSeq sorted_fanins;
  for(Vertex *vertex : fanins)
    sorted_fanins.push_back(vertex);

  sort(sorted_fanins, [&](Vertex *v1, Vertex *v2)
                      { float s1 = sta_->vertexSlack(v1, MinMax::min());
                        float s2 = sta_->vertexSlack(v2, MinMax::min());
                        if (fuzzyEqual(s1, s2)) {
                          float gap1 = slackGap(v1);
                          float gap2 = slackGap(v2);
                          // Break ties based on the hold/setup gap.
                          if (fuzzyEqual(gap1, gap2))
                            return v1->level() > v2->level();
                          else
                            return gap1 > gap2;
                        }
                        else
                          return s1 < s2;});
  if (debug_->check("repair_hold", 4)) {
    printf("Sorted fanins\n");
    printf("     hold_slack  slack_gap  level\n");
    for(Vertex *vertex : sorted_fanins)
      printf("%s %s %s %d\n",
             vertex->name(network_),
             units_->timeUnit()->asString(sta_->vertexSlack(vertex, MinMax::min()), 3),
             units_->timeUnit()->asString(slackGap(vertex), 3),
             vertex->level());
  }
  return sorted_fanins;
}

void
Resizer::makeHoldDelay(Vertex *drvr,
                       int buffer_count,
                       PinSeq &load_pins,
                       LibertyCell *buffer_cell)
{
  Pin *drvr_pin = drvr->pin();
  Instance *parent = db_network_->topInstance();
  Net *drvr_net = network_->isTopLevelPort(drvr_pin)
    ? db_network_->net(db_network_->term(drvr_pin))
    : db_network_->net(drvr_pin);
  Net *in_net = drvr_net;
  Net *out_net = nullptr;

  // Spread buffers between driver and load center.
  Point drvr_loc = db_network_->location(drvr_pin);
  Point load_center = findCenter(load_pins);
  int dx = (drvr_loc.x() - load_center.x()) / (buffer_count + 1);
  int dy = (drvr_loc.y() - load_center.y()) / (buffer_count + 1);

  // drvr_pin->drvr_net->hold_buffer->net2->load_pins
  for (int i = 0; i < buffer_count; i++) {
    string out_net_name = makeUniqueNetName();
    out_net = db_network_->makeNet(out_net_name.c_str(), parent);
    // drvr_pin->drvr_net->hold_buffer->net2->load_pins
    string buffer_name = makeUniqueInstName("hold");
    Instance *buffer = db_network_->makeInstance(buffer_cell,
                                                 buffer_name.c_str(),
                                                 parent);
    inserted_buffer_count_++;
    design_area_ += area(db_network_->cell(buffer_cell));

    LibertyPort *input, *output;
    buffer_cell->bufferPorts(input, output);
    sta_->connectPin(buffer, input, in_net);
    sta_->connectPin(buffer, output, out_net);
    Point buffer_loc(drvr_loc.x() + dx * i,
                     drvr_loc.y() + dy * i);
    setLocation(buffer, buffer_loc);
    in_net = out_net;
  }

  for (Pin *load_pin : load_pins) {
    Instance *load = db_network_->instance(load_pin);
    Port *load_port = db_network_->port(load_pin);
    sta_->disconnectPin(load_pin);
    sta_->connectPin(load, load_port, out_net);
  }
  if (have_estimated_parasitics_) {
    estimateWireParasitic(drvr_net);
    estimateWireParasitic(out_net);
  }
}

Point
Resizer::findCenter(PinSeq &pins)
{
  Point sum(0, 0);
  for (Pin *pin : pins) {
    Point loc = db_network_->location(pin);
    sum.x() += loc.x();
    sum.y() += loc.y();
  }
  return Point(sum.x() / pins.size(), sum.y() / pins.size());
}

// Gap between min setup and hold slacks.
// This says how much head room there is for adding delay to fix a
// hold violation before violating a setup check.
Slack
Resizer::slackGap(Slacks &slacks)
{
  return min(slacks[RiseFall::riseIndex()][MinMax::maxIndex()]
             - slacks[RiseFall::riseIndex()][MinMax::minIndex()],
             slacks[RiseFall::fallIndex()][MinMax::maxIndex()]
             - slacks[RiseFall::fallIndex()][MinMax::minIndex()]);
}

Slack
Resizer::slackGap(Vertex *vertex)
{
  Slacks slacks;
  sta_->vertexSlacks(vertex, slacks);
  return slackGap(slacks);
}

Slack
Resizer::holdSlack(Slacks &slacks)
{
  return min(slacks[RiseFall::riseIndex()][MinMax::minIndex()],
             slacks[RiseFall::fallIndex()][MinMax::minIndex()]);
}

Slack
Resizer::setupSlack(Slacks &slacks)
{
  return min(slacks[RiseFall::riseIndex()][MinMax::maxIndex()],
             slacks[RiseFall::fallIndex()][MinMax::maxIndex()]);
}

int
Resizer::fanout(Vertex *vertex)
{
  int fanout = 0;
  VertexOutEdgeIterator edge_iter(vertex, graph_);
  while (edge_iter.hasNext()) {
    edge_iter.next();
    fanout++;
  }
  return fanout;
}

////////////////////////////////////////////////////////////////

// Repair long wires, max slew, max capacitance, max fanout violations
// The whole enchilada.
void
Resizer::repairDesign(double max_wire_length, // meters
                      LibertyCell *buffer_cell)
{
  init();
  sta_->checkSlewLimitPreamble();
  sta_->checkCapacitanceLimitPreamble();
  sta_->checkFanoutLimitPreamble();

  inserted_buffer_count_ = 0;
  resize_count_ = 0;

  int repair_count = 0;
  int slew_violations = 0;
  int cap_violations = 0;
  int fanout_violations = 0;
  int length_violations = 0;
  int max_length = metersToDbu(max_wire_length);
  Level dcalc_valid_level = 0;
  for (int i = level_drvr_verticies_.size() - 1; i >= 0; i--) {
    Vertex *drvr = level_drvr_verticies_[i];
    Pin *drvr_pin = drvr->pin();
    Net *net = network_->net(drvr_pin);
    if (net
        && !sta_->isClock(drvr_pin)
        // Exclude tie hi/low cells.
        && !isFuncOneZero(drvr_pin)
        && !isSpecial(net)) {
      repairNet(net, drvr, true, true, true, max_length, true, buffer_cell,
                repair_count, slew_violations, cap_violations,
                fanout_violations, length_violations);
    }
  }
  ensureWireParasitics();

  if (slew_violations > 0)
    printf("Found %d slew violations.\n", slew_violations);
  if (fanout_violations > 0)
    printf("Found %d fanout violations.\n", fanout_violations);
  if (cap_violations > 0)
    printf("Found %d capacitance violations.\n", cap_violations);
  if (length_violations > 0)
    printf("Found %d long wires.\n", length_violations);
  if (inserted_buffer_count_ > 0) {
    printf("Inserted %d buffers in %d nets.\n",
           inserted_buffer_count_,
           repair_count);
    level_drvr_verticies_valid_ = false;
  }
  if (resize_count_ > 0)
    printf("Resized %d instances.\n", resize_count_);
}

// repairDesign but restricted to clock network and
// no max_fanout/max_cap checks.
void
Resizer::repairClkNets(double max_wire_length, // meters
                       LibertyCell *buffer_cell)
{
  init();
  // Need slews to resize inserted buffers.
  sta_->findDelays();

  inserted_buffer_count_ = 0;
  resize_count_ = 0;

  int repair_count = 0;
  int slew_violations = 0;
  int cap_violations = 0;
  int fanout_violations = 0;
  int length_violations = 0;
  int max_length = metersToDbu(max_wire_length);
  for (Clock *clk : sdc_->clks()) {
    for (const Pin *clk_pin : *sta_->pins(clk)) {
      if (network_->isDriver(clk_pin)) {
        Net *net = network_->isTopLevelPort(clk_pin)
          ? network_->net(network_->term(clk_pin))
          : network_->net(clk_pin);
        Vertex *drvr = graph_->pinDrvrVertex(clk_pin);
        // Do not resize clock tree gates.
        repairNet(net, drvr, false, false, false, max_length, false, buffer_cell,
                  repair_count, slew_violations, cap_violations,
                  fanout_violations, length_violations);
      }
    }
  }
  if (length_violations > 0)
    printf("Found %d long wires.\n", length_violations);
  if (inserted_buffer_count_ > 0) {
    printf("Inserted %d buffers in %d nets.\n",
           inserted_buffer_count_,
           repair_count);
    level_drvr_verticies_valid_ = false;
  }
}

// for debugging
void
Resizer::repairNet(Net *net,
                   double max_wire_length, // meters
                   LibertyCell *buffer_cell)
{
  init();

  sta_->checkSlewLimitPreamble();
  sta_->checkCapacitanceLimitPreamble();
  sta_->checkFanoutLimitPreamble();

  inserted_buffer_count_ = 0;
  resize_count_ = 0;
  resized_multi_output_insts_.clear();
  int repair_count = 0;
  int slew_violations = 0;
  int cap_violations = 0;
  int fanout_violations = 0;
  int length_violations = 0;
  int max_length = metersToDbu(max_wire_length);
  PinSet *drivers = network_->drivers(net);
  if (drivers && !drivers->empty()) {
    PinSet::Iterator drvr_iter(drivers);
    Pin *drvr_pin = drvr_iter.next();
    Vertex *drvr = graph_->pinDrvrVertex(drvr_pin);
    repairNet(net, drvr, true, true, true, max_length, true, buffer_cell,
              repair_count, slew_violations, cap_violations,
              fanout_violations, length_violations);
  }
  if (slew_violations > 0)
    printf("Found %d slew violations.\n", slew_violations);
  if (fanout_violations > 0)
    printf("Found %d fanout violations.\n", fanout_violations);
  if (cap_violations > 0)
    printf("Found %d capacitance violations.\n", cap_violations);
  if (length_violations > 0)
    printf("Found %d long wires.\n", length_violations);
  if (inserted_buffer_count_ > 0) {
    printf("Inserted %d buffers in %d nets.\n",
           inserted_buffer_count_,
           repair_count);
    level_drvr_verticies_valid_ = false;
  }
  printf("Resized %d instances.\n", resize_count_);
}

void
Resizer::repairNet(Net *net,
                   Vertex *drvr,
                   bool check_slew,
                   bool check_cap,
                   bool check_fanout,
                   int max_length, // dbu
                   bool resize_drvr,
                   LibertyCell *buffer_cell,
                   int &repair_count,
                   int &slew_violations,
                   int &cap_violations,
                   int &fanout_violations,
                   int &length_violations)
{
  SteinerTree *tree = makeSteinerTree(net, true, db_network_);
  if (tree) {
    Pin *drvr_pin = drvr->pin();
    debugPrint1(debug_, "repair_net", 1, "repair net %s\n",
                sdc_network_->pathName(drvr_pin));
    ensureWireParasitic(drvr_pin);
    graph_delay_calc_->findDelays(drvr);

    double max_cap = INF;
    float max_fanout = INF;
    bool repair_slew = false;
    bool repair_cap = false;
    bool repair_fanout = false;
    bool repair_wire = false;
    if (check_cap) {
      float cap, max_cap1, cap_slack;
      const Corner *corner1;
      const RiseFall *tr;
      sta_->checkCapacitance(drvr_pin, corner_, MinMax::max(),
                             corner1, tr, cap, max_cap1, cap_slack);
      if (cap_slack < 0.0) {
        max_cap = max_cap1;
        cap_violations++;
        repair_cap = true;
      }
    }
    if (check_fanout) {
      float fanout, fanout_slack;
      sta_->checkFanout(drvr_pin, MinMax::max(),
                        fanout, max_fanout, fanout_slack);
      if (fanout_slack < 0.0) {
        fanout_violations++;
        repair_fanout = true;
      }
    }
    int wire_length = findMaxSteinerDist(drvr, tree);
    if (max_length
        && wire_length > max_length) {
      length_violations++;
      repair_wire = true;
    }
    if (check_slew) {
      float slew, slew_slack, max_slew;
      checkSlew(drvr_pin, slew, max_slew, slew_slack);
      if (slew_slack < 0.0) {
        slew_violations++;
        LibertyPort *drvr_port = network_->libertyPort(drvr_pin);
        if (drvr_port) {
          // Find max load cap that corresponds to max_slew.
          double max_cap1 = findSlewLoadCap(drvr_port, max_slew);
          max_cap = min(max_cap, max_cap1);
          debugPrint1(debug_, "repair_net", 2, "slew max_cap=%s\n",
                      units_->capacitanceUnit()->asString(max_cap1, 3));
          repair_slew = true;
        }
      }
    }
    if (repair_slew
        || repair_cap
        || repair_fanout
        || repair_wire) {
      Point drvr_loc = db_network_->location(drvr->pin());
      debugPrint4(debug_, "repair_net", 1, "driver %s (%s %s) l=%s\n",
                  sdc_network_->pathName(drvr_pin),
                  units_->distanceUnit()->asString(dbuToMeters(drvr_loc.getX()), 1),
                  units_->distanceUnit()->asString(dbuToMeters(drvr_loc.getY()), 1),
                  units_->distanceUnit()->asString(dbuToMeters(wire_length), 1));
      SteinerPt drvr_pt = tree->steinerPt(drvr_pin);
      int ignore1;
      float ignore2, ignore3;
      PinSeq ignore4;
      repairNet(tree, drvr_pt, SteinerTree::null_pt, net,
                max_cap, max_fanout, max_length, buffer_cell, 0,
                ignore1, ignore2, ignore3, ignore4);
      repair_count++;
    }
    if (resize_drvr)
      resizeToTargetSlew(drvr_pin);
    delete tree;
  }
}

void
Resizer::checkSlew(const Pin *drvr_pin,
                   // Return values.
                   Slew &slew,
                   float &limit,
                   float &slack)
{
  slack = INF;
  PinConnectedPinIterator *pin_iter = network_->connectedPinIterator(drvr_pin);
  while (pin_iter->hasNext()) {
    Pin *pin = pin_iter->next();
    const Corner *corner1;
    const RiseFall *tr;
    Slew slew1;
    float limit1, slack1;
    sta_->checkSlew(pin, corner_, MinMax::max(), false,
                    corner1, tr, slew1, limit1, slack1);
    if (slack1 < slack) {
      slew = slew1;
      limit = limit1;
      slack = slack1;
    }
  }
  delete pin_iter;
}

// Find the output port load capacitance that results in slew.
double
Resizer::findSlewLoadCap(LibertyPort *drvr_port,
                         double slew)
{
  // cap1 lower bound
  // cap2 upper bound
  double cap1 = 0.0;
  double cap2 = slew / drvr_port->driveResistance() * 2;
  double tol = .01; // 1%
  double diff1 = gateSlewDiff(drvr_port, cap1, slew);
  double diff2 = gateSlewDiff(drvr_port, cap2, slew);
  // binary search for diff = 0.
  while (abs(cap1 - cap2) > max(cap1, cap2) * tol) {
    if (diff2 < 0.0) {
      cap1 = cap2;
      diff1 = diff2;
      cap2 *= 2;
      diff2 = gateSlewDiff(drvr_port, cap2, slew);
    }
    else {
      double cap3 = (cap1 + cap2) / 2.0;
      double diff3 = gateSlewDiff(drvr_port, cap3, slew);
      if (diff3 < 0.0) {
        cap1 = cap3;
        diff1 = diff3;
      }
      else {
        cap2 = cap3;
        diff2 = diff3;
      }
    }
  }
  return cap1;
}

// objective function
double
Resizer::gateSlewDiff(LibertyPort *drvr_port,
                      double load_cap,
                      double slew)
{
  ArcDelay delays[RiseFall::index_count];
  Slew slews[RiseFall::index_count];
  gateDelays(drvr_port, load_cap, delays, slews);
  Slew gate_slew = max(slews[RiseFall::riseIndex()], slews[RiseFall::fallIndex()]);
  return gate_slew - slew;
}

void
Resizer::repairNet(SteinerTree *tree,
                   SteinerPt pt,
                   SteinerPt prev_pt,
                   Net *net,
                   float max_cap,
                   float max_fanout,
                   int max_length, // dbu
                   LibertyCell *buffer_cell,
                   int level,
                   // Return values.
                   // Remaining parasiics after repeater insertion.
                   int &wire_length, // dbu
                   float &pin_cap,
                   float &fanout,
                   PinSeq &load_pins)
{
  Point pt_loc = tree->location(pt);
  int pt_x = pt_loc.getX();
  int pt_y = pt_loc.getY();
  debugPrint4(debug_, "repair_net", 2, "%*spt (%s %s)\n",
              level, "",
              units_->distanceUnit()->asString(dbuToMeters(pt_x), 1),
              units_->distanceUnit()->asString(dbuToMeters(pt_y), 1));
  SteinerPt left = tree->left(pt);
  int wire_length_left = 0;
  float pin_cap_left = 0.0;
  float fanout_left = 0.0;
  PinSeq loads_left;
  if (left != SteinerTree::null_pt)
    repairNet(tree, left, pt, net, max_cap, max_fanout, max_length,
              buffer_cell, level + 1,
              wire_length_left, pin_cap_left, fanout_left, loads_left);
  SteinerPt right = tree->right(pt);
  int wire_length_right = 0;
  float pin_cap_right = 0.0;
  float fanout_right = 0.0;
  PinSeq loads_right;
  if (right != SteinerTree::null_pt)
    repairNet(tree, right, pt, net, max_cap, max_fanout, max_length,
              buffer_cell, level + 1,
              wire_length_right, pin_cap_right, fanout_right, loads_right);
  debugPrint6(debug_, "repair_net", 3, "%*sleft l=%s cap=%s, right l=%s cap=%s\n",
              level, "",
              units_->distanceUnit()->asString(dbuToMeters(wire_length_left), 1),
              units_->capacitanceUnit()->asString(pin_cap_left, 2),
              units_->distanceUnit()->asString(dbuToMeters(wire_length_right), 1),
              units_->capacitanceUnit()->asString(pin_cap_right, 2));
  // Add a buffer to left or right branch to stay under the max cap/length/fanout.
  bool repeater_left = false;
  bool repeater_right = false;
  double cap_left = pin_cap_left + dbuToMeters(wire_length_left) * wire_cap_;
  double cap_right = pin_cap_right + dbuToMeters(wire_length_right) * wire_cap_;
  debugPrint4(debug_, "repair_net", 3, "%*scap_left=%s, right_cap=%s\n",
              level, "",
              units_->capacitanceUnit()->asString(cap_left, 2),
              units_->capacitanceUnit()->asString(cap_right, 2));
  bool cap_violation = (cap_left + cap_right) > max_cap;
  if (cap_violation) {
    debugPrint2(debug_, "repair_net", 3, "%*scap violation\n", level, "");
    if (cap_left > cap_right)
      repeater_left = true;
    else
      repeater_right = true;
  }
  bool length_violation = max_length > 0
    && (wire_length_left + wire_length_right) > max_length;
  if (length_violation) {
    debugPrint2(debug_, "repair_net", 3, "%*slength violation\n", level, "");
    if (wire_length_left > wire_length_right)
      repeater_left = true;
    else
      repeater_right = true;
  }
  bool fanout_violation = max_fanout > 0
    && (fanout_left + fanout_right) > max_fanout;
  if (fanout_violation) {
    debugPrint2(debug_, "repair_net", 3, "%*sfanout violation\n", level, "");
    if (fanout_left > fanout_right)
      repeater_left = true;
    else
      repeater_right = true;
  }

  if (repeater_left)
    makeRepeater(tree, pt, net, buffer_cell, level,
                 wire_length_left, pin_cap_left, fanout_left, loads_left);
  if (repeater_right)
    makeRepeater(tree, pt, net, buffer_cell, level,
                 wire_length_right, pin_cap_right, fanout_right, loads_right);

  wire_length = wire_length_left + wire_length_right;
  pin_cap = pin_cap_left + pin_cap_right;
  fanout = fanout_left + fanout_right;

  // Union left/right load pins.
  load_pins = loads_left;
  for (Pin *load_pin : loads_right)
    load_pins.push_back(load_pin);

  Net *buffer_out = nullptr;
  // Steiner pt pin is the net driver if prev_pt is null.
  if (prev_pt != SteinerTree::null_pt) {
    Pin *load_pin = tree->pin(pt);
    if (load_pin) {
      Point load_loc = db_network_->location(load_pin);
      debugPrint5(debug_, "repair_net", 2, "%*sload %s (%s %s)\n",
                  level, "",
                  sdc_network_->pathName(load_pin),
                  units_->distanceUnit()->asString(dbuToMeters(load_loc.getX()), 1),
                  units_->distanceUnit()->asString(dbuToMeters(load_loc.getY()), 1));
      LibertyPort *load_port = network_->libertyPort(load_pin);
      if (load_port) {
        pin_cap += portCapacitance(load_port);
        fanout += portFanoutLoad(load_port);
      }
      else
        fanout += 1;
      load_pins.push_back(load_pin);
    }

    Point prev_loc = tree->location(prev_pt);
    int length = Point::manhattanDistance(prev_loc, pt_loc);
    wire_length += length;
    // Back up from pt to prev_pt adding repeaters every max_length.
    int prev_x = prev_loc.getX();
    int prev_y = prev_loc.getY();
    debugPrint4(debug_, "repair_net", 3, "%*swl=%s l=%s\n",
                level, "",
                units_->distanceUnit()->asString(dbuToMeters(wire_length), 1),
                units_->distanceUnit()->asString(dbuToMeters(length), 1));
    while ((max_length > 0 && wire_length > max_length)
           || (wire_cap_ > 0.0
               && pin_cap < max_cap
               && (pin_cap + dbuToMeters(wire_length) * wire_cap_) > max_cap)) {
      // Make the wire a bit shorter than necessary to allow for
      // offset from instance origin to pin and detailed placement movement.
      double length_margin = .05;
      // Distance from pt to repeater backward toward prev_pt.
      double buf_dist;
      if (max_length > 0 && wire_length > max_length) {
        buf_dist = length - (wire_length - max_length * (1.0 - length_margin));
      }
      else if (wire_cap_ > 0.0
               && (pin_cap + dbuToMeters(wire_length) * wire_cap_) > max_cap) {
        int cap_length = metersToDbu((max_cap - pin_cap) / wire_cap_);
        buf_dist = length - (wire_length - cap_length * (1.0 - length_margin));
      }
      else
        internalError("how did I get here?");
      double dx = prev_x - pt_x;
      double dy = prev_y - pt_y;
      double d = buf_dist / length;
      int buf_x = pt_x + d * dx;
      int buf_y = pt_y + d * dy;
      makeRepeater(buf_x, buf_y, net, buffer_cell, level,
                   wire_length, pin_cap, fanout, load_pins);
      // Update for the next round.
      length -= buf_dist;
      wire_length = length;
      pt_x = buf_x;
      pt_y = buf_y;
      debugPrint4(debug_, "repair_net", 3, "%*swl=%s l=%s\n",
                  level, "",
                  units_->distanceUnit()->asString(dbuToMeters(wire_length), 1),
                  units_->distanceUnit()->asString(dbuToMeters(length), 1));
    }
  }
}

void
Resizer::makeRepeater(SteinerTree *tree,
                      SteinerPt pt,
                      Net *in_net,
                      LibertyCell *buffer_cell,
                      int level,
                      int &wire_length,
                      float &pin_cap,
                      float &fanout,
                      PinSeq &load_pins)
{
  Point pt_loc = tree->location(pt);
  makeRepeater(pt_loc.getX(), pt_loc.getY(), in_net, buffer_cell, level,
               wire_length, pin_cap, fanout, load_pins);
}

void
Resizer::makeRepeater(int x,
                      int y,
                      Net *in_net,
                      LibertyCell *buffer_cell,
                      int level,
                      int &wire_length,
                      float &pin_cap,
                      float &fanout,
                      PinSeq &load_pins)
{
  Point buf_loc(x, y);
  if (!core_exists_
      || core_.overlaps(buf_loc)) {
    LibertyPort *buffer_input_port, *buffer_output_port;
    buffer_cell->bufferPorts(buffer_input_port, buffer_output_port);

    string buffer_name = makeUniqueInstName("repeater");
    debugPrint5(debug_, "repair_net", 2, "%*s%s (%s %s)\n",
                level, "",
                buffer_name.c_str(),
                units_->distanceUnit()->asString(dbuToMeters(x), 1),
                units_->distanceUnit()->asString(dbuToMeters(y), 1));

    string buffer_out_name = makeUniqueNetName();
    Instance *parent = db_network_->topInstance();
    Net *buffer_out = db_network_->makeNet(buffer_out_name.c_str(), parent);
    dbNet *buffer_out_db = db_network_->staToDb(buffer_out);
    dbNet *in_net_db = db_network_->staToDb(in_net);
    buffer_out_db->setSigType(in_net_db->getSigType());
    Instance *buffer = db_network_->makeInstance(buffer_cell,
                                                 buffer_name.c_str(),
                                                 parent);
    setLocation(buffer, buf_loc);
    design_area_ += area(db_network_->cell(buffer_cell));
    inserted_buffer_count_++;

    sta_->connectPin(buffer, buffer_input_port, in_net);
    sta_->connectPin(buffer, buffer_output_port, buffer_out);

    for (Pin *load_pin : load_pins) {
      Port *load_port = network_->port(load_pin);
      Instance *load = network_->instance(load_pin);
      sta_->disconnectPin(load_pin);
      sta_->connectPin(load, load_port, buffer_out);
    }

    // Delete estimated parasitics on upstream driver.
    debugPrint1(debug_, "resizer_parasitics", 1, "delete parasitic %s\n",
                network_->pathName(in_net));
    parasitics_->deleteParasitics(in_net, parasitics_ap_);

    // Resize repeater as we back up by levels.
    Pin *drvr_pin = network_->findPin(buffer, buffer_output_port);
    resizeToTargetSlew(drvr_pin);
    buffer_cell = network_->libertyCell(buffer);
    buffer_cell->bufferPorts(buffer_input_port, buffer_output_port);

    Pin *buf_in_pin = network_->findPin(buffer, buffer_input_port);
    load_pins.clear();
    load_pins.push_back(buf_in_pin);
    wire_length = 0;
    pin_cap = portCapacitance(buffer_input_port);
    fanout = portFanoutLoad(buffer_input_port);
  }
}

////////////////////////////////////////////////////////////////

void
Resizer::reportLongWires(int count,
                         int digits)
{
  graph_ = sta_->ensureGraph();
  sta_->ensureClkNetwork();
  VertexSeq drvrs;
  findLongWires(drvrs);
  report_->print("Driver    length delay\n");
  int i = 0;
  for (Vertex *drvr : drvrs) {
    Pin *drvr_pin = drvr->pin();
    if (!network_->isTopLevelPort(drvr_pin)) {
      double wire_length = dbuToMeters(maxLoadManhattenDistance(drvr));
      double steiner_length = dbuToMeters(findMaxSteinerDist(drvr));
      double delay = wire_length * wire_res_ * wire_length * wire_cap_ * 0.5;
      report_->print("%s manhtn %s steiner %s %s\n",
                     sdc_network_->pathName(drvr_pin),
                     units_->distanceUnit()->asString(wire_length, 1),
                     units_->distanceUnit()->asString(steiner_length, 1),
                     units_->timeUnit()->asString(delay, digits));
      if (i == count)
        break;
      i++;
    }
  }
}

typedef std::pair<Vertex*, int> DrvrDist;

void
Resizer::findLongWires(VertexSeq &drvrs)
{
  Vector<DrvrDist> drvr_dists;
  VertexIterator vertex_iter(graph_);
  while (vertex_iter.hasNext()) {
    Vertex *vertex = vertex_iter.next();
    if (vertex->isDriver(network_)) {
      Pin *pin = vertex->pin();
      Net *net = network_->net(pin);
      // Hands off the clock nets.
      if (!sta_->isClock(pin)
          && !vertex->isConstant()
          && !vertex->isDisabledConstraint())
        drvr_dists.push_back(DrvrDist(vertex, maxLoadManhattenDistance(vertex)));
    }
  }
  sort(drvr_dists, [this](const DrvrDist &drvr_dist1,
                         const DrvrDist &drvr_dist2) {
                    return drvr_dist1.second > drvr_dist2.second;
                  });
  drvrs.reserve(drvr_dists.size());
  for (DrvrDist &drvr_dist : drvr_dists)
    drvrs.push_back(drvr_dist.first);
}

void
Resizer::findLongWiresSteiner(VertexSeq &drvrs)
{
  Vector<DrvrDist> drvr_dists;
  VertexIterator vertex_iter(graph_);
  while (vertex_iter.hasNext()) {
    Vertex *vertex = vertex_iter.next();
    if (vertex->isDriver(network_)) {
      Pin *pin = vertex->pin();
      Net *net = network_->net(pin);
      // Hands off the clock nets.
      if (!sta_->isClock(pin)
          && !vertex->isConstant())
        drvr_dists.push_back(DrvrDist(vertex, findMaxSteinerDist(vertex)));
    }
  }
  sort(drvr_dists, [this](const DrvrDist &drvr_dist1,
                          const DrvrDist &drvr_dist2) {
                     return drvr_dist1.second > drvr_dist2.second;
                   });
  drvrs.reserve(drvr_dists.size());
  for (DrvrDist &drvr_dist : drvr_dists)
    drvrs.push_back(drvr_dist.first);
}

// Find the maximum distance along steiner tree branches from
// the driver to loads (in dbu).
int
Resizer::findMaxSteinerDist(Vertex *drvr)
{
  Pin *drvr_pin = drvr->pin();
  Net *net = network_->net(drvr_pin);
  SteinerTree *tree = makeSteinerTree(net, true, db_network_);
  if (tree) {
    int dist = findMaxSteinerDist(drvr, tree);
    delete tree;
    return dist;
  }
  return 0;
}

int
Resizer::findMaxSteinerDist(Vertex *drvr,
                            SteinerTree *tree)
{
  Pin *drvr_pin = drvr->pin();
  SteinerPt drvr_pt = tree->steinerPt(drvr_pin);
  return findMaxSteinerDist(tree, drvr_pt, 0);
}

// DFS of steiner tree.
int
Resizer::findMaxSteinerDist(SteinerTree *tree,
                            SteinerPt pt,
                            int dist_from_drvr)
{
  Pin *pin = tree->pin(pt);
  if (pin && db_network_->isLoad(pin))
    return dist_from_drvr;
  else {
    Point loc = tree->location(pt);
    SteinerPt left = tree->left(pt);
    int left_max = 0;
    if (left != SteinerTree::null_pt) {
      int left_dist = Point::manhattanDistance(loc, tree->location(left));
      left_max = findMaxSteinerDist(tree, left, dist_from_drvr + left_dist);
    }
    SteinerPt right = tree->right(pt);
    int right_max = 0;
    if (right != SteinerTree::null_pt) {
      int right_dist = Point::manhattanDistance(loc, tree->location(right));
      right_max = findMaxSteinerDist(tree, right, dist_from_drvr + right_dist);
    }
    return max(left_max, right_max);
  }
}

double
Resizer::maxLoadManhattenDistance(const Net *net)
{
  NetPinIterator *pin_iter = network_->pinIterator(net);
  int max_dist = 0;
  while (pin_iter->hasNext()) {
    Pin *pin = pin_iter->next();
    if (network_->isDriver(pin)) {
      Vertex *drvr = graph_->pinDrvrVertex(pin);
      if (drvr) {
        int dist = maxLoadManhattenDistance(drvr);
        max_dist = max(max_dist, dist);
      }
    }
  }
  delete pin_iter;
  return dbuToMeters(max_dist);
}

int
Resizer::maxLoadManhattenDistance(Vertex *drvr)
{
  int max_dist = 0;
  Point drvr_loc = db_network_->location(drvr->pin());
  VertexOutEdgeIterator edge_iter(drvr, graph_);
  while (edge_iter.hasNext()) {
    Edge *edge = edge_iter.next();
    Vertex *load = edge->to(graph_);
    Point load_loc = db_network_->location(load->pin());
    int dist = Point::manhattanDistance(drvr_loc, load_loc);
    max_dist = max(max_dist, dist);
  }
  return max_dist;
}

////////////////////////////////////////////////////////////////

NetSeq *
Resizer::findFloatingNets()
{
  NetSeq *floating_nets = new NetSeq;
  NetIterator *net_iter = network_->netIterator(network_->topInstance());
  while (net_iter->hasNext()) {
    Net *net = net_iter->next();
    PinSeq loads;
    PinSeq drvrs;
    PinSet visited_drvrs;
    FindNetDrvrLoads visitor(nullptr, visited_drvrs, loads, drvrs, network_);
    network_->visitConnectedPins(net, visitor);
    if (drvrs.size() == 0 && loads.size() > 0)
      floating_nets->push_back(net);
  }
  delete net_iter;
  sort(floating_nets, sta::NetPathNameLess(network_));
  return floating_nets;
}

////////////////////////////////////////////////////////////////

string
Resizer::makeUniqueNetName()
{
  string node_name;
  Instance *top_inst = network_->topInstance();
  do 
    stringPrint(node_name, "net%d", unique_net_index_++);
  while (network_->findNet(top_inst, node_name.c_str()));
  return node_name;
}

string
Resizer::makeUniqueInstName(const char *base_name)
{
  return makeUniqueInstName(base_name, false);
}

string
Resizer::makeUniqueInstName(const char *base_name,
                            bool underscore)
{
  string inst_name;
  do 
    stringPrint(inst_name, underscore ? "%s_%d" : "%s%d",
                base_name, unique_inst_index_++);
  while (network_->findInstance(inst_name.c_str()));
  return inst_name;
}

float
Resizer::bufferInputCapacitance(LibertyCell *buffer_cell)
{
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);
  return portCapacitance(input);
}

float
Resizer::pinCapacitance(const Pin *pin)
{
  LibertyPort *port = network_->libertyPort(pin);
  if (port)
    return portCapacitance(port);
  else
    return 0.0;
}

float
Resizer::portCapacitance(const LibertyPort *port)
{
  float cap1 = port->capacitance(RiseFall::rise(), min_max_);
  float cap2 = port->capacitance(RiseFall::fall(), min_max_);
  return max(cap1, cap2);
}

float
Resizer::portFanoutLoad(LibertyPort *port)
{
  float fanout_load;
  bool exists;
  port->fanoutLoad(fanout_load, exists);
  if (!exists) {
    LibertyLibrary *lib = port->libertyLibrary();
    lib->defaultFanoutLoad(fanout_load, exists);
  }
  if (exists)
    return fanout_load;
  else
    return 0.0;
}

Requireds
Resizer::pinRequireds(const Pin *pin)
{
  Vertex *vertex = graph_->pinLoadVertex(pin);
  PathAnalysisPt *path_ap = corner_->findPathAnalysisPt(min_max_);
  Requireds requireds;
  for (RiseFall *rf : RiseFall::range()) {
    int rf_index = rf->index();
    Required required = sta_->vertexRequired(vertex, rf, path_ap);
    if (fuzzyInf(required))
      // Unconstrained pin.
      required = 0.0;
    requireds[rf_index] = required;
  }
  return requireds;
}

float
Resizer::bufferDelay(LibertyCell *buffer_cell,
                     RiseFall *rf,
                     float load_cap)
{
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);
  ArcDelay gate_delays[RiseFall::index_count];
  Slew slews[RiseFall::index_count];
  gateDelays(output, load_cap, gate_delays, slews);
  return gate_delays[rf->index()];
}

float
Resizer::bufferDelay(LibertyCell *buffer_cell,
                     float load_cap)
{
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);
  ArcDelay gate_delays[RiseFall::index_count];
  Slew slews[RiseFall::index_count];
  gateDelays(output, load_cap, gate_delays, slews);
  return max(gate_delays[RiseFall::riseIndex()],
             gate_delays[RiseFall::fallIndex()]);
}

// Self delay; buffer -> buffer
float
Resizer::bufferDelay(LibertyCell *buffer_cell)
{
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);
  ArcDelay gate_delays[RiseFall::index_count];
  Slew slews[RiseFall::index_count];
  float load_cap = input->capacitance();
  gateDelays(output, load_cap, gate_delays, slews);
  return max(gate_delays[RiseFall::riseIndex()],
             gate_delays[RiseFall::fallIndex()]);
}

float
Resizer::bufferDelay(LibertyCell *buffer_cell,
                     const RiseFall *rf)
{
  LibertyPort *input, *output;
  buffer_cell->bufferPorts(input, output);
  ArcDelay gate_delays[RiseFall::index_count];
  Slew slews[RiseFall::index_count];
  float load_cap = input->capacitance();
  gateDelays(output, load_cap, gate_delays, slews);
  return gate_delays[rf->index()];
}

// Rise/fall delays across all timing arcs into drvr_port.
// Uses target slew for input slew.
void
Resizer::gateDelays(LibertyPort *drvr_port,
                    float load_cap,
                    // Return values.
                    ArcDelay delays[RiseFall::index_count],
                    Slew slews[RiseFall::index_count])
{
  for (auto rf_index : RiseFall::rangeIndex()) {
    delays[rf_index] = -INF;
    slews[rf_index] = -INF;
  }
  LibertyCell *cell = drvr_port->libertyCell();
  LibertyCellTimingArcSetIterator set_iter(cell);
  while (set_iter.hasNext()) {
    TimingArcSet *arc_set = set_iter.next();
    if (arc_set->to() == drvr_port) {
      TimingArcSetArcIterator arc_iter(arc_set);
      while (arc_iter.hasNext()) {
        TimingArc *arc = arc_iter.next();
        RiseFall *in_rf = arc->fromTrans()->asRiseFall();
        int out_rf_index = arc->toTrans()->asRiseFall()->index();
        float in_slew = tgt_slews_[in_rf->index()];
        ArcDelay gate_delay;
        Slew drvr_slew;
        arc_delay_calc_->gateDelay(cell, arc, in_slew, load_cap,
                                   nullptr, 0.0, pvt_, dcalc_ap_,
                                   gate_delay,
                                   drvr_slew);
        delays[out_rf_index] = max(delays[out_rf_index], gate_delay);
        slews[out_rf_index] = max(slews[out_rf_index], drvr_slew);
      }
    }
  }
}

////////////////////////////////////////////////////////////////

// Find the max wire length before it is faster to split the wire
// in half with a buffer (in meters).
double
Resizer::findMaxWireLength(LibertyCell *buffer_cell)
{
  LibertyPort *load_port, *drvr_port;
  buffer_cell->bufferPorts(load_port, drvr_port);
  return findMaxWireLength(drvr_port);
}

double
Resizer::findMaxWireLength(LibertyPort *drvr_port)
{
  LibertyCell *cell = drvr_port->libertyCell();
  double drvr_r = drvr_port->driveResistance();
  // wire_length1 lower bound
  // wire_length2 upper bound
  double wire_length1 = 0.0;
  // Initial guess with wire resistance same as driver resistance.
  double wire_length2 = drvr_r / wire_res_;
  double tol = .01; // 1%
  double diff1 = splitWireDelayDiff(wire_length1, cell);
  double diff2 = splitWireDelayDiff(wire_length2, cell);
  // binary search for diff = 0.
  while (abs(wire_length1 - wire_length2) > max(wire_length1, wire_length2) * tol) {
    if (diff2 < 0.0) {
      wire_length1 = wire_length2;
      diff1 = diff2;
      wire_length2 *= 2;
      diff2 = splitWireDelayDiff(wire_length2, cell);
    }
    else {
      double wire_length3 = (wire_length1 + wire_length2) / 2.0;
      double diff3 = splitWireDelayDiff(wire_length3, cell);
      if (diff3 < 0.0) {
        wire_length1 = wire_length3;
        diff1 = diff3;
      }
      else {
        wire_length2 = wire_length3;
        diff2 = diff3;
      }
    }
  }
  return wire_length1;
}

// objective function
double
Resizer::splitWireDelayDiff(double wire_length,
                            LibertyCell *buffer_cell)
{
  Delay delay1, delay2;
  Slew slew1, slew2;
  bufferWireDelay(buffer_cell, wire_length, delay1, slew1);
  bufferWireDelay(buffer_cell, wire_length / 2, delay2, slew2);
  return delay1 - delay2 * 2;
}

void
Resizer::bufferWireDelay(LibertyCell *buffer_cell,
                         double wire_length, // meters
                         // Return values.
                         Delay &delay,
                         Slew &slew)
{
  LibertyPort *load_port, *drvr_port;
  buffer_cell->bufferPorts(load_port, drvr_port);
  return cellWireDelay(drvr_port, load_port, wire_length, delay, slew);
}

// Cell delay plus wire delay.
// Uses target slew for input slew.
// drvr_port and load_port do not have to be the same liberty cell.
void
Resizer::cellWireDelay(LibertyPort *drvr_port,
                       LibertyPort *load_port,
                       double wire_length, // meters
                       // Return values.
                       Delay &delay,
                       Slew &slew)
{
  Instance *top_inst = network_->topInstance();
  // Tmp net for parasitics to live on.
  Net *net = sta_->makeNet("wire", top_inst);
  LibertyCell *drvr_cell = drvr_port->libertyCell();
  LibertyCell *load_cell = load_port->libertyCell();
  Instance *drvr = sta_->makeInstance("drvr", drvr_cell, top_inst);
  Instance *load = sta_->makeInstance("load", load_cell, top_inst);
  sta_->connectPin(drvr, drvr_port, net);
  sta_->connectPin(load, load_port, net);
  Pin *drvr_pin = network_->findPin(drvr, drvr_port);
  Pin *load_pin = network_->findPin(load, load_port);

  Parasitic *parasitic = makeWireParasitic(net, drvr_pin, load_pin, wire_length);
  // Let delay calc reduce parasitic network as it sees fit.
  Parasitic *drvr_parasitic = arc_delay_calc_->findParasitic(drvr_pin, RiseFall::rise(),
                                                             dcalc_ap_);

  // Max rise/fall delays.
  delay = -INF;
  slew = -INF;
  LibertyCellTimingArcSetIterator set_iter(drvr_cell);
  while (set_iter.hasNext()) {
    TimingArcSet *arc_set = set_iter.next();
    if (arc_set->to() == drvr_port) {
      TimingArcSetArcIterator arc_iter(arc_set);
      while (arc_iter.hasNext()) {
        TimingArc *arc = arc_iter.next();
        RiseFall *in_rf = arc->fromTrans()->asRiseFall();
        int out_rf_index = arc->toTrans()->asRiseFall()->index();
        double in_slew = tgt_slews_[in_rf->index()];
        ArcDelay gate_delay;
        Slew drvr_slew;
        arc_delay_calc_->gateDelay(drvr_cell, arc, in_slew, 0.0,
                                   drvr_parasitic, 0.0, pvt_, dcalc_ap_,
                                   gate_delay,
                                   drvr_slew);
        ArcDelay wire_delay;
        Slew load_slew;
        arc_delay_calc_->loadDelay(load_pin, wire_delay, load_slew);
        delay = max(delay, gate_delay + wire_delay);
        slew = max(slew, load_slew);
      }
    }
  }
  // Cleanup the turds.
  arc_delay_calc_->finishDrvrPin();
  parasitics_->deleteParasiticNetwork(net, dcalc_ap_->parasiticAnalysisPt());
  sta_->deleteInstance(drvr);
  sta_->deleteInstance(load);
  sta_->deleteNet(net);
}

Parasitic *
Resizer::makeWireParasitic(Net *net,
                           Pin *drvr_pin,
                           Pin *load_pin,
                           double wire_length) // meters
{
  Parasitic *parasitic = parasitics_->makeParasiticNetwork(net, false,
                                                           parasitics_ap_);
  ParasiticNode *n1 = parasitics_->ensureParasiticNode(parasitic, drvr_pin);
  ParasiticNode *n2 = parasitics_->ensureParasiticNode(parasitic, load_pin);
  double wire_cap = wire_length * wire_cap_;
  double wire_res = wire_length * wire_res_;
  parasitics_->incrCap(n1, wire_cap / 2.0, parasitics_ap_);
  parasitics_->makeResistor(nullptr, n1, n2, wire_res, parasitics_ap_);
  parasitics_->incrCap(n2, wire_cap / 2.0, parasitics_ap_);
  return parasitic;
}

////////////////////////////////////////////////////////////////

double
Resizer::findMaxSlewWireLength(LibertyPort *drvr_port,
                               LibertyPort *load_port,
                               double max_slew)
{
  // wire_length1 lower bound
  // wire_length2 upper bound
  double wire_length1 = 0.0;
  double wire_length2 = std::sqrt(max_slew / (wire_res_ * wire_cap_));
  double tol = .01; // 1%
  double diff1 = maxSlewWireDiff(drvr_port, load_port, wire_length1, max_slew);
  double diff2 = maxSlewWireDiff(drvr_port, load_port, wire_length2, max_slew);
  // binary search for diff = 0.
  while (abs(wire_length1 - wire_length2) > max(wire_length1, wire_length2) * tol) {
    if (diff2 < 0.0) {
      wire_length1 = wire_length2;
      diff1 = diff2;
      wire_length2 *= 2;
      diff2 = maxSlewWireDiff(drvr_port, load_port, wire_length2, max_slew);
    }
    else {
      double wire_length3 = (wire_length1 + wire_length2) / 2.0;
      double diff3 = maxSlewWireDiff(drvr_port, load_port, wire_length3, max_slew);
      if (diff3 < 0.0) {
        wire_length1 = wire_length3;
        diff1 = diff3;
      }
      else {
        wire_length2 = wire_length3;
        diff2 = diff3;
      }
    }
  }
  return wire_length1;
}

// objective function
double
Resizer::maxSlewWireDiff(LibertyPort *drvr_port,
                         LibertyPort *load_port,
                         double wire_length,
                         double max_slew)
{
  Delay delay;
  Slew slew;
  cellWireDelay(drvr_port, load_port, wire_length, delay, slew);
  return slew - max_slew;
}

////////////////////////////////////////////////////////////////

double
Resizer::designArea()
{
  ensureBlock();
  return design_area_;
}

void
Resizer::designAreaIncr(float delta) {
  design_area_ += delta;
}

double
Resizer::findDesignArea()
{
  double design_area = 0.0;
  for (dbInst *inst : block_->getInsts()) {
    dbMaster *master = inst->getMaster();
    design_area += area(master);
  }
  return design_area;
}

int
Resizer::fanout(Pin *drvr_pin)
{
  int fanout = 0;
  NetConnectedPinIterator *pin_iter = network_->connectedPinIterator(drvr_pin);
  while (pin_iter->hasNext()) {
    Pin *pin = pin_iter->next();
    if (pin != drvr_pin)
      fanout++;
  }
  delete pin_iter;
  return fanout;
}

bool
Resizer::isFuncOneZero(const Pin *drvr_pin)
{
  LibertyPort *port = network_->libertyPort(drvr_pin);
  if (port) {
    FuncExpr *func = port->function();
    return func && (func->op() == FuncExpr::op_zero
                    || func->op() == FuncExpr::op_one);
  }
  return false;
}

bool
Resizer::isSpecial(Net *net)
{
  dbNet *db_net = db_network_->staToDb(net);
  return db_net->isSpecial();
}

void
Resizer::writeNetSVG(Net *net,
                     const char *filename)
{
  SteinerTree *tree = makeSteinerTree(net, true, db_network_);
  if (tree)
    tree->writeSVG(sdc_network_, filename);
}

////////////////////////////////////////////////////////////////

void
Resizer::repairClkInverters()
{
  // Abbreviated copyState
  db_network_ = sta_->getDbNetwork();
  sta_->ensureLevelized();
  graph_ = sta_->graph();
  ensureBlock();
  InstanceSeq clk_inverters;
  findClkInverters(clk_inverters);
  for (Instance *inv : clk_inverters)
    cloneClkInverter(inv);
}

void
Resizer::findClkInverters(// Return values
                          InstanceSeq &clk_inverters)
{
  ClkArrivalSearchPred srch_pred(this);
  BfsFwdIterator bfs(BfsIndex::other, &srch_pred, this);
  for (Clock *clk : sdc_->clks()) {
    for (Pin *pin : clk->leafPins()) {
      Vertex *vertex = graph_->pinDrvrVertex(pin);
      bfs.enqueue(vertex);
    }
  }
  while (bfs.hasNext()) {
    Vertex *vertex = bfs.next();
    const Pin *pin = vertex->pin();
    Instance *inst = network_->instance(pin);
    LibertyCell *lib_cell = network_->libertyCell(inst);
    if (vertex->isDriver(network_)
        && lib_cell
        && lib_cell->isInverter()) {
      clk_inverters.push_back(inst);
      debugPrint1(debug_, "repair_clk_inverters", 2, "inverter %s\n",
                  network_->pathName(inst));
    }
    if (!vertex->isRegClk())
      bfs.enqueueAdjacentVertices(vertex);
  }
}

void
Resizer::cloneClkInverter(Instance *inv)
{
  LibertyCell *inv_cell = network_->libertyCell(inv);
  LibertyPort *in_port, *out_port;
  inv_cell->bufferPorts(in_port, out_port);
  Pin *in_pin = network_->findPin(inv, in_port);
  Pin *out_pin = network_->findPin(inv, out_port);
  Net *in_net = network_->net(in_pin);
  dbNet *in_net_db = db_network_->staToDb(in_net);
  Net *out_net = network_->isTopLevelPort(out_pin)
    ? network_->net(network_->term(out_pin))
    : network_->net(out_pin);
  if (out_net) {
    const char *inv_name = network_->name(inv);
    Instance *top_inst = network_->topInstance();
    NetConnectedPinIterator *load_iter = network_->pinIterator(out_net);
    while (load_iter->hasNext()) {
      Pin *load_pin = load_iter->next();
      if (load_pin != out_pin) {
        string clone_name = makeUniqueInstName(inv_name, true);
        Instance *clone = sta_->makeInstance(clone_name.c_str(),
                                             inv_cell, top_inst);
        Point clone_loc = db_network_->location(load_pin);
        setLocation(clone, clone_loc);

        string clone_out_net_name = makeUniqueNetName();
        Net *clone_out_net = db_network_->makeNet(clone_out_net_name.c_str(), top_inst);
        dbNet *clone_out_net_db = db_network_->staToDb(clone_out_net);
        clone_out_net_db->setSigType(in_net_db->getSigType());

        Instance *load = network_->instance(load_pin);
        sta_->connectPin(clone, in_port, in_net);
        sta_->connectPin(clone, out_port, clone_out_net);

        // Connect load to clone
        sta_->disconnectPin(load_pin);
        Port *load_port = network_->port(load_pin);
        sta_->connectPin(load, load_port, clone_out_net);
      }
    }
    delete load_iter;

    // Delete inv
    sta_->disconnectPin(in_pin);
    sta_->disconnectPin(out_pin);
    sta_->deleteNet(out_net);
    sta_->deleteInstance(inv);
  }
}

} // namespace sta
