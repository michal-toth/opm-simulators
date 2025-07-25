/*
  Copyright 2013, 2015, 2020 SINTEF Digital, Mathematics and Cybernetics.
  Copyright 2015 Andreas Lauser
  Copyright 2017 IRIS

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_SIMULATOR_FULLY_IMPLICIT_BLACKOIL_HEADER_INCLUDED
#define OPM_SIMULATOR_FULLY_IMPLICIT_BLACKOIL_HEADER_INCLUDED

#include <opm/common/ErrorMacros.hpp>

#if HAVE_MPI
#define RESERVOIR_COUPLING_ENABLED
#endif
#ifdef RESERVOIR_COUPLING_ENABLED
#include <opm/input/eclipse/Schedule/ResCoup/ReservoirCouplingInfo.hpp>
#include <opm/input/eclipse/Schedule/ResCoup/MasterGroup.hpp>
#include <opm/input/eclipse/Schedule/ResCoup/Slaves.hpp>
#include <opm/simulators/flow/ReservoirCouplingMaster.hpp>
#include <opm/simulators/flow/ReservoirCouplingSlave.hpp>
#include <opm/common/Exceptions.hpp>
#endif

#include <opm/input/eclipse/Units/UnitSystem.hpp>

#include <opm/grid/utility/StopWatch.hpp>

#include <opm/simulators/aquifers/BlackoilAquiferModel.hpp>
#include <opm/simulators/flow/BlackoilModel.hpp>
#include <opm/simulators/flow/BlackoilModelParameters.hpp>
#include <opm/simulators/flow/ConvergenceOutputConfiguration.hpp>
#include <opm/simulators/flow/ExtraConvergenceOutputThread.hpp>
#include <opm/simulators/flow/NonlinearSolver.hpp>
#include <opm/simulators/flow/SimulatorConvergenceOutput.hpp>
#include <opm/simulators/flow/SimulatorReportBanners.hpp>
#include <opm/simulators/flow/SimulatorSerializer.hpp>
#include <opm/simulators/timestepping/AdaptiveTimeStepping.hpp>
#include <opm/simulators/timestepping/ConvergenceReport.hpp>
#include <opm/simulators/utils/moduleVersion.hpp>
#include <opm/simulators/wells/WellState.hpp>

#if HAVE_HDF5
#include <opm/simulators/utils/HDF5Serializer.hpp>
#endif

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace Opm::Parameters {

struct EnableAdaptiveTimeStepping { static constexpr bool value = true; };
struct OutputExtraConvergenceInfo { static constexpr auto* value = "none"; };
struct SaveStep { static constexpr auto* value = ""; };
struct SaveFile { static constexpr auto* value = ""; };
struct LoadFile { static constexpr auto* value = ""; };
struct LoadStep { static constexpr int value = -1; };
struct Slave { static constexpr bool value = false; };

} // namespace Opm::Parameters

namespace Opm::detail {

void registerSimulatorParameters();

}

namespace Opm {

/// a simulator for the blackoil model
template<class TypeTag>
class SimulatorFullyImplicitBlackoil : private SerializableSim
{
protected:
    struct MPI_Comm_Deleter;
public:
    using Simulator = GetPropType<TypeTag, Properties::Simulator>;
    using Grid = GetPropType<TypeTag, Properties::Grid>;
    using FluidSystem = GetPropType<TypeTag, Properties::FluidSystem>;
    using ElementContext = GetPropType<TypeTag, Properties::ElementContext>;
    using BlackoilIndices = GetPropType<TypeTag, Properties::Indices>;
    using PrimaryVariables = GetPropType<TypeTag, Properties::PrimaryVariables>;
    using MaterialLaw = GetPropType<TypeTag, Properties::MaterialLaw>;
    using SolutionVector = GetPropType<TypeTag, Properties::SolutionVector>;
    using MaterialLawParams = GetPropType<TypeTag, Properties::MaterialLawParams>;
    using AquiferModel = GetPropType<TypeTag, Properties::AquiferModel>;
    using Model = GetPropType<TypeTag, Properties::NonlinearSystem>;

    using TimeStepper = AdaptiveTimeStepping<TypeTag>;
    using PolymerModule = BlackOilPolymerModule<TypeTag>;
    using MICPModule = BlackOilMICPModule<TypeTag>;


    using Solver = NonlinearSolver<TypeTag, Model>;
    using ModelParameters = typename Model::ModelParameters;
    using SolverParameters = typename Solver::SolverParameters;
    using WellModel = BlackoilWellModel<TypeTag>;

    /// Initialise from parameters and objects to observe.
    /// \param[in] param       parameters, this class accepts the following:
    ///     parameter (default)            effect
    ///     -----------------------------------------------------------
    ///     output (true)                  write output to files?
    ///     output_dir ("output")          output directoty
    ///     output_interval (1)            output every nth step
    ///     nl_pressure_residual_tolerance (0.0) pressure solver residual tolerance (in Pascal)
    ///     nl_pressure_change_tolerance (1.0)   pressure solver change tolerance (in Pascal)
    ///     nl_pressure_maxiter (10)       max nonlinear iterations in pressure
    ///     nl_maxiter (30)                max nonlinear iterations in transport
    ///     nl_tolerance (1e-9)            transport solver absolute residual tolerance
    ///     num_transport_substeps (1)     number of transport steps per pressure step
    ///     use_segregation_split (false)  solve for gravity segregation (if false,
    ///                                    segregation is ignored).
    ///
    /// \param[in] props         fluid and rock properties
    /// \param[in] linsolver     linear solver
    /// \param[in] eclipse_state the object which represents an internalized ECL deck
    /// \param[in] output_writer
    /// \param[in] threshold_pressures_by_face   if nonempty, threshold pressures that inhibit flow
    explicit SimulatorFullyImplicitBlackoil(Simulator& simulator)
        : simulator_(simulator)
        , serializer_(*this,
                      FlowGenericVanguard::comm(),
                      simulator_.vanguard().eclState().getIOConfig(),
                      Parameters::Get<Parameters::SaveStep>(),
                      Parameters::Get<Parameters::LoadStep>(),
                      Parameters::Get<Parameters::SaveFile>(),
                      Parameters::Get<Parameters::LoadFile>())
    {
        phaseUsage_ = phaseUsageFromDeck(eclState());

        // Only rank 0 does print to std::cout, and only if specifically requested.
        this->terminalOutput_ = false;
        if (this->grid().comm().rank() == 0) {
            this->terminalOutput_ = Parameters::Get<Parameters::EnableTerminalOutput>();

            auto getPhaseName = ConvergenceOutputThread::ComponentToPhaseName {
                [compNames = typename Model::ComponentName{}](const int compIdx)
                { return std::string_view { compNames.name(compIdx) }; }
            };

            if (!simulator_.vanguard().eclState().getIOConfig().initOnly()) {
                this->convergence_output_.
                    startThread(this->simulator_.vanguard().eclState(),
                                Parameters::Get<Parameters::OutputExtraConvergenceInfo>(),
                                R"(OutputExtraConvergenceInfo (--output-extra-convergence-info))",
                                getPhaseName);
            }
        }
    }

    ~SimulatorFullyImplicitBlackoil() override
    {
        // Safe to call on all ranks, not just the I/O rank.
        convergence_output_.endThread();
    }

    static void registerParameters()
    {
        ModelParameters::registerParameters();
        SolverParameters::registerParameters();
        TimeStepper::registerParameters();
        detail::registerSimulatorParameters();
    }

    /// Run the simulation.
    /// This will run succesive timesteps until timer.done() is true. It will
    /// modify the reservoir and well states.
    /// \param[in,out] timer       governs the requested reporting timesteps
    /// \param[in,out] state       state of reservoir: pressure, fluxes
    /// \return                    simulation report, with timing data
#ifdef RESERVOIR_COUPLING_ENABLED
    SimulatorReport run(SimulatorTimer& timer, int argc, char** argv)
    {
        init(timer, argc, argv);
#else
    SimulatorReport run(SimulatorTimer& timer)
    {
        init(timer);
#endif
        // Make cache up to date. No need for updating it in elementCtx.
        // NB! Need to be at the correct step in case of restart
        simulator_.setEpisodeIndex(timer.currentStepNum());
        simulator_.model().invalidateAndUpdateIntensiveQuantities(/*timeIdx=*/0);
        // Main simulation loop.
        while (!timer.done()) {
            simulator_.problem().writeReports(timer);
            bool continue_looping = runStep(timer);
            if (!continue_looping) break;
        }
        simulator_.problem().writeReports(timer);
        return finalize();
    }

#ifdef RESERVOIR_COUPLING_ENABLED
    // This method should only be called if slave mode (i.e. Parameters::Get<Parameters::Slave>())
    // is false. We try to determine if this is a normal flow simulation or a reservoir
    // coupling master. It is a normal flow simulation if the schedule does not contain
    // any SLAVES and GRUPMAST keywords.
    bool checkRunningAsReservoirCouplingMaster()
    {
        for (std::size_t report_step = 0; report_step < this->schedule().size(); ++report_step) {
            auto rescoup = this->schedule()[report_step].rescoup();
            auto slave_count = rescoup.slaveCount();
            auto master_group_count = rescoup.masterGroupCount();
            // - GRUPMAST and SLAVES keywords need to be specified at the same report step
            // - They can only occur once in the schedule
            if (slave_count > 0 && master_group_count > 0) {
                return true;
            }
            else if (slave_count > 0 && master_group_count == 0) {
                throw ReservoirCouplingError(
                    "Inconsistent reservoir coupling master schedule: "
                    "Slave count is greater than 0 but master group count is 0"
                );
            }
            else if (slave_count == 0 && master_group_count > 0) {
                throw ReservoirCouplingError(
                    "Inconsistent reservoir coupling master schedule: "
                    "Master group count is greater than 0 but slave count is 0"
                );
            }
        }
        return false;
    }
#endif

#ifdef RESERVOIR_COUPLING_ENABLED
    // NOTE: The argc and argv will be used when launching a slave process
    void init(const SimulatorTimer& timer, int argc, char** argv)
    {
        auto slave_mode = Parameters::Get<Parameters::Slave>();
        if (slave_mode) {
            this->reservoirCouplingSlave_ =
                std::make_unique<ReservoirCouplingSlave>(
                    FlowGenericVanguard::comm(),
                    this->schedule(), timer
                );
            this->reservoirCouplingSlave_->sendAndReceiveInitialData();
            this->simulator_.setReservoirCouplingSlave(this->reservoirCouplingSlave_.get());
            wellModel_().setReservoirCouplingSlave(this->reservoirCouplingSlave_.get());
        }
        else {
            auto master_mode = checkRunningAsReservoirCouplingMaster();
            if (master_mode) {
                this->reservoirCouplingMaster_ =
                    std::make_unique<ReservoirCouplingMaster>(
                        FlowGenericVanguard::comm(),
                        this->schedule(),
                        argc, argv
                    );
                this->simulator_.setReservoirCouplingMaster(this->reservoirCouplingMaster_.get());
                wellModel_().setReservoirCouplingMaster(this->reservoirCouplingMaster_.get());
            }
        }
#else
    void init(const SimulatorTimer& timer)
    {
#endif
        simulator_.setEpisodeIndex(-1);

        // Create timers and file for writing timing info.
        solverTimer_ = std::make_unique<time::StopWatch>();
        totalTimer_ = std::make_unique<time::StopWatch>();
        totalTimer_->start();

        // adaptive time stepping
        bool enableAdaptive = Parameters::Get<Parameters::EnableAdaptiveTimeStepping>();
        bool enableTUNING = Parameters::Get<Parameters::EnableTuning>();
        if (enableAdaptive) {
            const UnitSystem& unitSystem = this->simulator_.vanguard().eclState().getUnits();
            const auto& sched_state = schedule()[timer.currentStepNum()];
            auto max_next_tstep = sched_state.max_next_tstep(enableTUNING);
            if (enableTUNING) {
                adaptiveTimeStepping_ = std::make_unique<TimeStepper>(max_next_tstep,
                                                                      sched_state.tuning(),
                                                                      unitSystem, report_, terminalOutput_);
            }
            else {
                adaptiveTimeStepping_ = std::make_unique<TimeStepper>(unitSystem, report_, max_next_tstep, terminalOutput_);
            }
            if (isRestart()) {
                // For restarts the simulator may have gotten some information
                // about the next timestep size from the OPMEXTRA field
                adaptiveTimeStepping_->setSuggestedNextStep(simulator_.timeStepSize());
            }
        }
    }

    void updateTUNING(const Tuning& tuning)
    {
        modelParam_.tolerance_mb_ = tuning.XXXMBE;
        if (terminalOutput_) {
            OpmLog::debug(fmt::format("Setting SimulatorFullyImplicitBlackoil mass balance limit (XXXMBE) to {:.2e}", tuning.XXXMBE));
        }
    }

    bool runStep(SimulatorTimer& timer)
    {
        if (schedule().exitStatus().has_value()) {
            if (terminalOutput_) {
                OpmLog::info("Stopping simulation since EXIT was triggered by an action keyword.");
            }
            report_.success.exit_status = schedule().exitStatus().value();
            return false;
        }

        if (serializer_.shouldLoad()) {
            serializer_.loadTimerInfo(timer);
        }

        // Report timestep.
        if (terminalOutput_) {
            std::ostringstream ss;
            timer.report(ss);
            OpmLog::debug(ss.str());
            details::outputReportStep(timer);
        }

        // write the inital state at the report stage
        if (timer.initialStep()) {
            Dune::Timer perfTimer;
            perfTimer.start();

            simulator_.setEpisodeIndex(-1);
            simulator_.setEpisodeLength(0.0);
            simulator_.setTimeStepSize(0.0);
            wellModel_().beginReportStep(timer.currentStepNum());
            simulator_.problem().writeOutput(true);

            report_.success.output_write_time += perfTimer.stop();
        }

        // Run a multiple steps of the solver depending on the time step control.
        solverTimer_->start();

        if (!solver_) {
            solver_ = createSolver(wellModel_());
        }

        simulator_.startNextEpisode(
            simulator_.startTime()
               + schedule().seconds(timer.currentStepNum()),
            timer.currentStepLength());
        simulator_.setEpisodeIndex(timer.currentStepNum());

        if (serializer_.shouldLoad()) {
            wellModel_().prepareDeserialize(serializer_.loadStep() - 1);
            serializer_.loadState();
            simulator_.model().invalidateAndUpdateIntensiveQuantities(/*timeIdx=*/0);
        }

        this->solver_->model().beginReportStep();

        const bool enableTUNING = Parameters::Get<Parameters::EnableTuning>();

        // If sub stepping is enabled allow the solver to sub cycle
        // in case the report steps are too large for the solver to converge
        //
        // \Note: The report steps are met in any case
        // \Note: The sub stepping will require a copy of the state variables
        if (adaptiveTimeStepping_) {
            auto tuningUpdater = [enableTUNING, this,
                                  reportStep = timer.currentStepNum()](const double curr_time,
                                                                       double dt, const int timeStep)
            {
                auto& schedule = this->simulator_.vanguard().schedule();
                auto& events = this->schedule()[reportStep].events();

                bool result = false;
                if (events.hasEvent(ScheduleEvents::TUNING_CHANGE)) {
                    // Unset the event to not trigger it again on the next sub step
                    schedule.clear_event(ScheduleEvents::TUNING_CHANGE, reportStep);
                    const auto& sched_state = schedule[reportStep];
                    const auto& max_next_tstep = sched_state.max_next_tstep(enableTUNING);
                    const auto& tuning = sched_state.tuning();

                    if (enableTUNING) {
                        adaptiveTimeStepping_->updateTUNING(max_next_tstep, tuning);
                        // \Note: Assumes TUNING is only used with adaptive time-stepping
                        // \Note: Need to update both solver (model) and simulator since solver is re-created each report step.
                        solver_->model().updateTUNING(tuning);
                        this->updateTUNING(tuning);
                        dt = this->adaptiveTimeStepping_->suggestedNextStep();
                    } else {
                        dt = max_next_tstep;
                        this->adaptiveTimeStepping_->updateNEXTSTEP(max_next_tstep);
                    }
                    result = max_next_tstep > 0;
                }

                const auto& wcycle = schedule[reportStep].wcycle.get();
                if (wcycle.empty()) {
                    return result;
                }

                const auto& wmatcher = schedule.wellMatcher(reportStep);
                double wcycle_time_step =
                    wcycle.nextTimeStep(curr_time,
                                        dt,
                                        wmatcher,
                                        this->wellModel_().wellOpenTimes(),
                                        this->wellModel_().wellCloseTimes(),
                                        [timeStep,
                                         &wg_events = this->wellModel_().reportStepStartEvents()]
                                        (const std::string& name)
                                        {
                                            if (timeStep != 0) {
                                                return false;
                                            }
                                            return wg_events.hasEvent(name, ScheduleEvents::REQUEST_OPEN_WELL);
                                        });

                wcycle_time_step = this->grid().comm().min(wcycle_time_step);
                if (dt != wcycle_time_step) {
                    this->adaptiveTimeStepping_->updateNEXTSTEP(wcycle_time_step);
                    return true;
                }

                return result;
            };

            tuningUpdater(timer.simulationTimeElapsed(),
                          this->adaptiveTimeStepping_->suggestedNextStep(), 0);

#ifdef RESERVOIR_COUPLING_ENABLED
            if (this->reservoirCouplingMaster_) {
                this->reservoirCouplingMaster_->maybeSpawnSlaveProcesses(timer.currentStepNum());
            }
            else if (this->reservoirCouplingSlave_) {
                this->reservoirCouplingSlave_->maybeActivate(timer.currentStepNum());
            }
#endif
            const auto& events = schedule()[timer.currentStepNum()].events();
            bool event = events.hasEvent(ScheduleEvents::NEW_WELL) ||
                events.hasEvent(ScheduleEvents::INJECTION_TYPE_CHANGED) ||
                events.hasEvent(ScheduleEvents::WELL_SWITCHED_INJECTOR_PRODUCER) ||
                events.hasEvent(ScheduleEvents::PRODUCTION_UPDATE) ||
                events.hasEvent(ScheduleEvents::INJECTION_UPDATE) ||
                events.hasEvent(ScheduleEvents::WELL_STATUS_CHANGE);
            auto stepReport = adaptiveTimeStepping_->step(timer, *solver_, event, tuningUpdater);
            report_ += stepReport;
            //Pass simulation report to eclwriter for summary output
            simulator_.problem().setSimulationReport(report_);
        } else {
            // solve for complete report step
            auto stepReport = solver_->step(timer, nullptr);
            report_ += stepReport;
            if (terminalOutput_) {
                std::ostringstream ss;
                stepReport.reportStep(ss);
                OpmLog::info(ss.str());
            }
        }

        // write simulation state at the report stage
        Dune::Timer perfTimer;
        perfTimer.start();
        const double nextstep = adaptiveTimeStepping_ ? adaptiveTimeStepping_->suggestedNextStep() : -1.0;
        simulator_.problem().setNextTimeStepSize(nextstep);
        simulator_.problem().writeOutput(true);
        report_.success.output_write_time += perfTimer.stop();

        solver_->model().endReportStep();

        // take time that was used to solve system for this reportStep
        solverTimer_->stop();

        // update timing.
        report_.success.solver_time += solverTimer_->secsSinceStart();

        if (this->grid().comm().rank() == 0) {
            // Grab the step convergence reports that are new since last we
            // were here.
            const auto& reps = this->solver_->model().stepReports();
            convergence_output_.write(reps);
        }

        // Increment timer, remember well state.
        ++timer;
        
        if (terminalOutput_) {
            std::string msg =
                "Time step took " + std::to_string(solverTimer_->secsSinceStart()) + " seconds; "
                "total solver time " + std::to_string(report_.success.solver_time) + " seconds.";
            OpmLog::debug(msg);
        }

        serializer_.save(timer);

        return true;
    }

    SimulatorReport finalize()
    {
        // make sure all output is written to disk before run is finished
        {
            Dune::Timer finalOutputTimer;
            finalOutputTimer.start();

            simulator_.problem().finalizeOutput();
            report_.success.output_write_time += finalOutputTimer.stop();
        }

        // Stop timer and create timing report
        totalTimer_->stop();
        report_.success.total_time = totalTimer_->secsSinceStart();
        report_.success.converged = true;

        return report_;
    }

    const Grid& grid() const
    { return simulator_.vanguard().grid(); }

    template<class Serializer>
    void serializeOp(Serializer& serializer)
    {
        serializer(simulator_);
        serializer(report_);
        serializer(adaptiveTimeStepping_);
    }

    const Model& model() const
    { return solver_->model(); }

protected:
    //! \brief Load simulator state from hdf5 serializer.
    void loadState([[maybe_unused]] HDF5Serializer& serializer,
                   [[maybe_unused]] const std::string& groupName) override
    {
#if HAVE_HDF5
        serializer.read(*this, groupName, "simulator_data");
#endif
    }

    //! \brief Save simulator state using hdf5 serializer.
    void saveState([[maybe_unused]] HDF5Serializer& serializer,
                   [[maybe_unused]] const std::string& groupName) const override
    {
#if HAVE_HDF5
        serializer.write(*this, groupName, "simulator_data");
#endif
    }

    //! \brief Returns header data
    std::array<std::string,5> getHeader() const override
    {
        std::ostringstream str;
        Parameters::printValues(str);
        return {"OPM Flow",
                moduleVersion(),
                compileTimestamp(),
                simulator_.vanguard().caseName(),
                str.str()};
    }

    //! \brief Returns local-to-global cell mapping.
    const std::vector<int>& getCellMapping() const override
    {
        return simulator_.vanguard().globalCell();
    }

    std::unique_ptr<Solver> createSolver(WellModel& wellModel)
    {
        auto model = std::make_unique<Model>(simulator_,
                                             modelParam_,
                                             wellModel,
                                             terminalOutput_);

        if (this->modelParam_.write_partitions_) {
            const auto& iocfg = this->eclState().cfg().io();

            const auto odir = iocfg.getOutputDir()
                / std::filesystem::path { "partition" }
                / iocfg.getBaseName();

            if (this->grid().comm().rank() == 0) {
                create_directories(odir);
            }

            this->grid().comm().barrier();

            model->writePartitions(odir);

            this->modelParam_.write_partitions_ = false;
        }

        return std::make_unique<Solver>(solverParam_, std::move(model));
    }

    const EclipseState& eclState() const
    { return simulator_.vanguard().eclState(); }


    const Schedule& schedule() const
    { return simulator_.vanguard().schedule(); }

    bool isRestart() const
    {
        const auto& initconfig = eclState().getInitConfig();
        return initconfig.restartRequested();
    }

    WellModel& wellModel_()
    { return simulator_.problem().wellModel(); }

    const WellModel& wellModel_() const
    { return simulator_.problem().wellModel(); }

    // Data.
    Simulator& simulator_;

    ModelParameters modelParam_;
    SolverParameters solverParam_;

    std::unique_ptr<Solver> solver_;

    // Observed objects.
    PhaseUsage phaseUsage_;
    // Misc. data
    bool terminalOutput_;

    SimulatorReport report_;
    std::unique_ptr<time::StopWatch> solverTimer_;
    std::unique_ptr<time::StopWatch> totalTimer_;
    std::unique_ptr<TimeStepper> adaptiveTimeStepping_;

    SimulatorConvergenceOutput convergence_output_{};

#ifdef RESERVOIR_COUPLING_ENABLED
    bool slaveMode_{false};
    std::unique_ptr<ReservoirCouplingMaster> reservoirCouplingMaster_{nullptr};
    std::unique_ptr<ReservoirCouplingSlave> reservoirCouplingSlave_{nullptr};
#endif

    SimulatorSerializer serializer_;
};

} // namespace Opm

#endif // OPM_SIMULATOR_FULLY_IMPLICIT_BLACKOIL_HEADER_INCLUDED
