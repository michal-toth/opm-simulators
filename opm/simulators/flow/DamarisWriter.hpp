// -*- mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
// vi: set et ts=4 sw=4 sts=4:
/*
  Copyright 2022 SINTEF Digital, Mathematics and Cybernetics.
  Copyright 2023 Inria, Bretagne–Atlantique Research Center
  
  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.

  Consult the COPYING file in the top-level source directory of this
  module for the precise wording of the license and the list of
  copyright holders.
*/
/*!
 * \file
 *
 * \copydoc Opm::DamarisWriter
 */
#ifndef OPM_DAMARIS_WRITER_HPP
#define OPM_DAMARIS_WRITER_HPP

#include <dune/grid/common/partitionset.hh>


#include <opm/common/OpmLog/OpmLog.hpp>

#include <opm/simulators/flow/countGlobalCells.hpp>
#include <opm/simulators/flow/DamarisProperties.hpp>
#include <opm/simulators/flow/EclGenericWriter.hpp>
#include <opm/simulators/flow/FlowBaseVanguard.hpp>
#include <opm/simulators/flow/OutputBlackoilModule.hpp>
#include <opm/simulators/utils/DamarisVar.hpp>
#include <opm/simulators/utils/DamarisKeywords.hpp>
#include <opm/simulators/utils/DeferredLoggingErrorHelpers.hpp>
#include <opm/simulators/utils/GridDataOutput.hpp>
#include <opm/simulators/utils/ParallelSerialization.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <memory>
#include <numeric>
#include <string>
#include <vector>
#include <unordered_set>


namespace Opm {

namespace DamarisOutput {

int endIteration(int rank);
int setParameter(const char* field, int rank, int value);
int setPosition(const char* field, int rank, int64_t pos);
int write(const char* field, int rank, const void* data);
int setupWritingPars(Parallel::Communication comm,
                     const int n_elements_local_grid,
                     std::vector<unsigned long long>& elements_rank_offsets);
}

/*!
 * \ingroup EclBlackOilSimulator
 *
 * \brief Collects necessary output values and pass them to Damaris server processes.
 *
 * Currently only passing through PRESSURE, GLOBAL_CELL_INDEX and MPI_RANK information.
 * This class now passes through the 3D mesh information to Damaris to enable
 * in situ visualization via Paraview or Ascent. And developed so that variables specified 
 * through the Eclipse input deck will be available to Damaris.
 */
 
 
template <class TypeTag>
class DamarisWriter : public EclGenericWriter<GetPropType<TypeTag, Properties::Grid>,
                                              GetPropType<TypeTag, Properties::EquilGrid>,
                                              GetPropType<TypeTag, Properties::GridView>,
                                              GetPropType<TypeTag, Properties::ElementMapper>,
                                              GetPropType<TypeTag, Properties::Scalar>>
{
    using Simulator = GetPropType<TypeTag, Properties::Simulator>;
    using GridView = GetPropType<TypeTag, Properties::GridView>;
    using Grid = GetPropType<TypeTag, Properties::Grid>;
    using EquilGrid = GetPropType<TypeTag, Properties::EquilGrid>;
    using Scalar = GetPropType<TypeTag, Properties::Scalar>;
    using ElementContext = GetPropType<TypeTag, Properties::ElementContext>;
    using Element = typename GridView::template Codim<0>::Entity;
    using ElementMapper = GetPropType<TypeTag, Properties::ElementMapper>;
    
    using BaseType = EclGenericWriter<Grid,EquilGrid,GridView,ElementMapper,Scalar>;
    using DamarisVarInt = DamarisOutput::DamarisVar<int>;
    using DamarisVarChar = DamarisOutput::DamarisVar<char>;
    using DamarisVarDbl = DamarisOutput::DamarisVar<double>;

public:
    static void registerParameters()
    {
        Parameters::registerParam<TypeTag, Properties::DamarisOutputHdfCollective>
            ("Write output via Damaris using parallel HDF5 to "
             "get single file and dataset per timestep instead "
             "of one per Damaris core with multiple datasets.");
        Parameters::registerParam<TypeTag, Properties::DamarisSaveToHdf>
            ("Set to false to prevent output to HDF5. "
             "Uses collective output by default or "
             "set --enable-damaris-collective=false to"
             "use file per core (file per Damaris server).");
        Parameters::registerParam<TypeTag, Properties::DamarisSaveMeshToHdf>
            ("Saves the mesh data to the HDF5 file (1st iteration only). "
             "Will set  --damaris-output-hdf-collective to false "
             "so will use file per core (file per Damaris server) output "
            "(global sizes and offset values  of mesh variables are not being provided as yet).");
        Parameters::registerParam<TypeTag, Properties::DamarisPythonScript>
            ("Set to the path and filename of a Python script to run on "
             "Damaris server resources with access to OPM flow data.");
        Parameters::registerParam<TypeTag, Properties::DamarisPythonParaviewScript>
            ("Set to the path and filename of a Paraview Python script "
             "to run on Paraview Catalyst (1 or 2) on Damaris server "
             "resources with access to OPM flow data.");
        Parameters::registerParam<TypeTag, Properties::DamarisSimName>
            ("The name of the simulation to be used by Damaris. "
             "If empty (the default) then Damaris uses \"opm-sim-<random-number>\". "
             "This name is used for the Damaris HDF5 file name prefix. "
             "Make unique if writing to the same output directory.");
        Parameters::registerParam<TypeTag, Properties::DamarisLogLevel>
            ("The log level for the Damaris logging system (boost log based). "
             "Levels are: [trace, debug, info, warning, error, fatal]. "
             "Currently debug and info are useful. ");
        Parameters::registerParam<TypeTag, Properties::DamarisDaskFile>
            ("The name of a Dask json configuration file (if using Dask for processing).");
        Parameters::registerParam<TypeTag, Properties::DamarisDedicatedCores>
            ("Set the number of dedicated cores (MPI processes) "
             "that should be used for Damaris processing (per node). "
             "Must divide evenly into the number of simulation ranks (client ranks).");
        Parameters::registerParam<TypeTag, Properties::DamarisDedicatedNodes>
            ("Set the number of dedicated nodes (full nodes) "
             "that should be used for Damaris processing (per simulation). "
             "Must divide evenly into the number of simulation nodes.");
        Parameters::registerParam<TypeTag, Properties::DamarisSharedMemorySizeBytes>
            ("Set the size of the shared memory buffer used for IPC "
             "between the simulation and the Damaris resources. "
             "Needs to hold all the variables published, possibly over "
             "multiple simulation iterations.");
        Parameters::registerParam<TypeTag, Properties::DamarisSharedMemoryName>
            ("The name of the shared memory area to be used by Damaris for the current. "
             "If empty (the default) then Damaris uses \"opm-damaris-<random-string>\". "
             "This name should be unique if multiple simulations are running on "
             "the same node/server as it is used for the Damaris shmem name and by "
             "the Python Dask library to locate sections of variables.");
        Parameters::registerParam<TypeTag, Properties::DamarisLimitVariables>
            ("A comma separated list of variable names that a user wants to pass"
             "through via DamarisOutput::DamarisWriter::writeOutput)() to the "
             "damaris_write() call. This can be used to limit the number of"
             "variables being passed to the Daamis plugins (Paraview, Python and HDF5)");
    }

    // The Simulator object should preferably have been const - the
    // only reason that is not the case is due to the SummaryState
    // object owned deep down by the vanguard.
    DamarisWriter(Simulator& simulator)
        : BaseType(simulator.vanguard().schedule(),
                   simulator.vanguard().eclState(),
                   simulator.vanguard().summaryConfig(),
                   simulator.vanguard().grid(),
                   ((simulator.vanguard().grid().comm().rank() == 0)
                    ? &simulator.vanguard().equilGrid()
                    : nullptr),
                   simulator.vanguard().gridView(),
                   simulator.vanguard().cartesianIndexMapper(),
                   ((simulator.vanguard().grid().comm().rank() == 0)
                    ? &simulator.vanguard().equilCartesianIndexMapper()
                    : nullptr),
                   false, false)
        , simulator_(simulator)
    {
        this->damarisUpdate_ = true ;

        this->rank_ = this->simulator_.vanguard().grid().comm().rank() ;
        this->nranks_ = this->simulator_.vanguard().grid().comm().size();

        this->elements_rank_offsets_.resize(this->nranks_);
        
        // Get the size of the unique vector elements (excludes the shared 'ghost' elements)
        //
        // Might possibly use
        //
        //     detail::countLocalInteriorCellsGridView(this->simulator_.gridView())
        //
        // from countGlobalCells.hpp instead of calling std::distance() directly.
        {
            const auto& gridView = this->simulator_.gridView();
            const auto& interior_elements = elements(gridView, Dune::Partitions::interior);

            this->numElements_ = std::distance(interior_elements.begin(), interior_elements.end());
        }

        if (this->nranks_ > 1) {
            auto smryCfg = (this->rank_ == 0)
                ? this->eclIO_->finalSummaryConfig()
                : SummaryConfig{};

            eclBroadcast(this->simulator_.vanguard().grid().comm(), smryCfg);

            this->damarisOutputModule_ = std::make_unique<OutputBlackOilModule<TypeTag>>
                (simulator, smryCfg, this->collectOnIORank_);
        }
        else {
            this->damarisOutputModule_ = std::make_unique<OutputBlackOilModule<TypeTag>>
                (simulator, this->eclIO_->finalSummaryConfig(), this->collectOnIORank_);
        }
        
        wanted_vars_set_ = Opm::DamarisOutput::getSetOfIncludedVariables<TypeTag>();
    }

    /*!
     * \brief Writes localCellData through to Damaris servers. Sets up the unstructured mesh which is passed to Damaris.
     */
    void writeOutput(data::Solution& localCellData , bool isSubStep)
    {
        OPM_TIMEBLOCK(writeOutput);
        const int reportStepNum = simulator_.episodeIndex() + 1;

        // added this as localCellData was not being written
        if (!isSubStep)
            this->damarisOutputModule_->invalidateLocalData() ;  
        this->prepareLocalCellData(isSubStep, reportStepNum);
        this->damarisOutputModule_->outputErrorLog(simulator_.gridView().comm());

        // The damarisWriter is not outputing well or aquifer data (yet)
        auto localWellData = simulator_.problem().wellModel().wellData(); // data::Well

        if (! isSubStep) 
        {
            if (localCellData.size() == 0) {
                this->damarisOutputModule_->assignToSolution(localCellData);
            }

            // add cell data to perforations for Rft output
            this->damarisOutputModule_->addRftDataToWells(localWellData, reportStepNum);
            
            // On first call and if the mesh and variable size change then set damarisUpdate_ to true
            if (damarisUpdate_ == true) {
                // Sets the damaris parameter values "n_elements_local" and "n_elements_total" 
                // which define sizes of the Damaris variables, per-rank and globally (over all ranks).
                // Also sets the offsets to where a ranks array data sits within the global array. 
                // This is usefull for HDF5 output and for defining distributed arrays in Dask.
                dam_err_ = DamarisOutput::setupWritingPars(simulator_.vanguard().grid().comm(),
                                                           numElements_, elements_rank_offsets_);
                
                // sets positions and data for non-time-varying variables MPI_RANK and GLOBAL_CELL_INDEX
                this->setGlobalIndexForDamaris() ; 
                
                // Set the geometry data for the mesh model.
                // this function writes the mesh data directly to Damaris shared memory using Opm::DamarisOutput::DamarisVar objects.
                this->writeDamarisGridOutput() ;
                
                // Currently by default we assume a static mesh grid (the geometry unchanging through the simulation)
                // Set damarisUpdate_ to true if we want to update the geometry sent to Damaris 
                this->damarisUpdate_ = false; 
            }

            /*if (this->damarisOutputModule_->getPRESSURE_ptr() != nullptr) 
            {
                dam_err_ = DamarisOutput::setPosition("PRESSURE", rank_,
                                                      this->elements_rank_offsets_[rank_]);
                dam_err_ = DamarisOutput::write("PRESSURE", rank_,
                                                this->damarisOutputModule_->getPRESSURE_ptr());

                dam_err_ =  DamarisOutput::endIteration(rank_);
            }*/
            
            
            // Call damaris_set_position() for all available variables
            // There is an assumption that all variables are the same size, with the same offset.
            // see initDamarisTemplateXmlFile.cpp for the Damaris XML descriptions.
            for ( auto damVar : localCellData ) {
                // std::map<std::string, data::CellData>
                const std::string name = damVar.first ;
                dam_err_ = DamarisOutput::setPosition(name.c_str(), rank_,
                                                      this->elements_rank_offsets_[rank_]);
            }

            // Call damaris_write() for all available variables
            for ( auto damVar : localCellData ) 
            {
               // std::map<std::string, data::CellData>
              const std::string& name = damVar.first ;
              
              std::unordered_set<std::string>::const_iterator is_in_set = wanted_vars_set_.find ( name );
              
              if ((is_in_set != wanted_vars_set_.end() ) || (wanted_vars_set_.size() == 0)) {
                  data::CellData  dataCol = damVar.second ;
                  OpmLog::debug(fmt::format("Name of Damaris Variable       : ( rank:{})  name: {}  ",  rank_, name));
                  
                  // It does not seem I can test for what type of data is present (double or int)
                  // in the std::variant within the data::CellData, so I will use a try catch block. 
                  // Although, only MPI_RANK and GLOBAL_CELL_INDEX are set as integer types (in the 
                  // XML file) so it is a moot point. 
                  // We could use damaris_get_type() to check what the type is specified as
                  // within the Damaris XML file
                  try {
                    if (dataCol.data<double>().size() >= this->numElements_) {
                        dam_err_ = DamarisOutput::write(name.c_str(), rank_,
                                                        dataCol.data<double>().data()) ;
                    }
                  }
                  catch (std::bad_variant_access const& ex) {
                    // Not a std::vector<double>
                    if (dataCol.data<int>().size() >= this->numElements_) {
                        dam_err_ = DamarisOutput::write(name.c_str(), rank_,
                                                      dataCol.data<int>().data()) ;
                    }
                  }
              }
              

            }
            
           /*   
            Code for when we want to pass to Damaris the single cell 'block' data variables
            auto mybloc = damarisOutputModule_->getBlockData() ;
            for ( auto damVar : mybloc ) {
               // std::map<std::string, data::CellData>
              const std::string name = std::get<0>(damVar.first) ;
              const int part = std::get<1>(damVar.first) ;
              double  dataCol = damVar.second ;
              std::cout << "Name of Damaris Block Varaiable : (" << rank_ << ")  "  << name  << "  part : " << part << "  Value : "  << dataCol <<  std::endl ;  
            } 
            
            dam_err_ =  DamarisOutput::endIteration(rank_);
            */
            if (this->damarisOutputModule_->getPRESSURE_ptr() != nullptr) 
            {
                dam_err_ =  DamarisOutput::endIteration(rank_);
            }
            
         } // end of ! isSubstep
    }

private:
    int dam_err_ ;
    int rank_  ;       
    int nranks_ ;
    int numElements_ ;  ///<  size of the unique vector elements
    std::unordered_set<std::string> wanted_vars_set_ ;
    
    Simulator& simulator_;
    std::unique_ptr<OutputBlackOilModule<TypeTag>> damarisOutputModule_;
    std::vector<unsigned long long> elements_rank_offsets_ ;
    bool damarisUpdate_ = false;  ///< Whenever this is true writeOutput() will set up Damaris mesh information and offsets of model fields

    static bool enableDamarisOutput_()
    {
        return Parameters::get<TypeTag, Properties::EnableDamarisOutput>();
    }

    void setGlobalIndexForDamaris () 
    {
        // Use damaris_set_position to set the offset in the global size of the array.
        // This is used so that output functionality (e.g. HDF5Store) knows global offsets of the data of the ranks
        // setPosition("PRESSURE", comm.rank(), elements_rank_offsets_[comm.rank()]);
        dam_err_ = DamarisOutput::setPosition("GLOBAL_CELL_INDEX", rank_, elements_rank_offsets_[rank_]);

        // Set the size of the MPI variable
        // N.B. MPI_RANK is only saved to HDF5 if --damaris-save-mesh-to-hdf=true is specified
        DamarisVarInt mpi_rank_var(1, {"n_elements_local"}, "MPI_RANK", rank_);
        mpi_rank_var.setDamarisPosition({static_cast<int64_t>(elements_rank_offsets_[rank_])});
    
        // GLOBAL_CELL_INDEX is used to reorder variable data when writing to disk 
        // This is enabled using select-file="GLOBAL_CELL_INDEX" in the <variable> XML tag
        if (this->collectOnIORank_.isParallel()) {
            const std::vector<int>& local_to_global =
                this->collectOnIORank_.localIdxToGlobalIdxMapping();
            dam_err_ = DamarisOutput::write("GLOBAL_CELL_INDEX", rank_, local_to_global.data());
        } else {
            std::vector<int> local_to_global_filled ;
            local_to_global_filled.resize(this->numElements_) ;
            std::iota(local_to_global_filled.begin(), local_to_global_filled.end(), 0);
            dam_err_ = DamarisOutput::write("GLOBAL_CELL_INDEX", rank_, local_to_global_filled.data());
        }

        // This is an example of writing to the Damaris shared memory directly (i.e. not using 
        // damaris_write() to copy data there)
        // We will add the MPI rank value directly into shared memory using the DamarisVar 
        // wrapper of the C based Damaris API.
        // The shared memory is given back to Damaris when the DamarisVarInt goes out of scope.
        // DamarisVarInt mpi_rank_var_test(1, {"n_elements_local"},  "MPI_RANK", rank_);
        mpi_rank_var.setDamarisParameterAndShmem( {this->numElements_ } ) ;
        // Fill the created memory area
        std::fill(mpi_rank_var.data(), mpi_rank_var.data() + numElements_, rank_);
        
       
    }

    void writeDamarisGridOutput()
    {
        const auto& gridView = simulator_.gridView();
        GridDataOutput::SimMeshDataAccessor geomData(gridView, Dune::Partitions::interior) ;

        try {
            const bool hasPolyCells = geomData.polyhedralCellPresent() ;
            if ( hasPolyCells ) {
                OpmLog::error(fmt::format("ERORR: rank {} The DUNE geometry grid has polyhedral elements - These elements are currently not supported.", rank_ ));
            }

            // This is the template XML model for x,y,z coordinates defined in initDamarisXmlFile.cpp which is used to 
            // build the internally generated Damaris XML configuration file.
            // <parameter name="n_coords_local"     type="int" value="1" />
            // <parameter name="n_coords_global"    type="int" value="1" comment="only needed if we need to write to HDF5 in Collective mode"/>
            // <layout    name="n_coords_layout"    type="double" dimensions="n_coords_local"   comment="For the individual x, y and z coordinates of the mesh vertices"  />
            // <group name="coordset/coords/values"> 
            //     <variable name="x"    layout="n_coords_layout"  type="scalar"  visualizable="false"  unit="m"   script="PythonConduitTest" time-varying="false" />
            //     <variable name="y"    layout="n_coords_layout"  type="scalar"  visualizable="false"  unit="m"   script="PythonConduitTest" time-varying="false" />
            //     <variable name="z"    layout="n_coords_layout"  type="scalar"  visualizable="false"  unit="m"   script="PythonConduitTest" time-varying="false" />
            // </group>

            DamarisVarDbl  var_x(1, {"n_coords_local"}, "coordset/coords/values/x", rank_) ;
            // N.B. We have not set any position/offset values (using DamarisVar::SetDamarisPosition). 
            // They are not needed for mesh data as each process has a local geometric model. 
            // However, HDF5 collective and Dask arrays cannot be used for this data.
            var_x.setDamarisParameterAndShmem( { geomData.getNVertices() } ) ;
             
            DamarisVarDbl var_y(1, {"n_coords_local"}, "coordset/coords/values/y", rank_) ;
            var_y.setDamarisParameterAndShmem( { geomData.getNVertices() } ) ;
             
            DamarisVarDbl  var_z(1, {"n_coords_local"}, "coordset/coords/values/z", rank_) ;
            var_z.setDamarisParameterAndShmem( { geomData.getNVertices() } ) ;
            
            // Now we can use the shared memory area that Damaris has allocated and use it to write the x,y,z coordinates
            if ( geomData.writeGridPoints(var_x, var_y, var_z) < 0)
                 DUNE_THROW(Dune::IOError, geomData.getError()  );
            
            //  This is the template XML model for connectivity, offsets and types, as defined in initDamarisXmlFile.cpp which is used to 
            //  build the internally generated Damaris XML configuration file.
            // <parameter name="n_connectivity_ph"        type="int"  value="1" />
            // <layout    name="n_connections_layout_ph"  type="int"  dimensions="n_connectivity_ph"   comment="Layout for connectivities "  />
            // <parameter name="n_offsets_types_ph"       type="int"  value="1" />
            // <layout    name="n_offsets_layout_ph"      type="int"  dimensions="n_offsets_types_ph+1"  comment="Layout for the offsets_ph"  />
            // <layout    name="n_types_layout_ph"        type="char" dimensions="n_offsets_types_ph"  comment="Layout for the types_ph "  />
            // <group name="topologies/topo/elements">
            //     <variable name="connectivity" layout="n_connections_layout_ph"  type="scalar"  visualizable="false"  unit=""   script="PythonConduitTest" time-varying="false" />
            //     <variable name="offsets"      layout="n_offsets_layout_ph"    type="scalar"  visualizable="false"  unit=""   script="PythonConduitTest" time-varying="false" />
            //     <variable name="types"        layout="n_types_layout_ph"    type="scalar"  visualizable="false"  unit=""   script="PythonConduitTest" time-varying="false" />
            // </group>

            DamarisVarInt var_connectivity(1, {"n_connectivity_ph"},
                                           "topologies/topo/elements/connectivity", rank_) ;
            var_connectivity.setDamarisParameterAndShmem({ geomData.getNCorners()}) ;
            DamarisVarInt  var_offsets(1, {"n_offsets_types_ph"},
                                      "topologies/topo/elements/offsets", rank_) ;
            var_offsets.setDamarisParameterAndShmem({ geomData.getNCells()+1}) ;
            DamarisVarChar  var_types(1, {"n_offsets_types_ph"},
                                     "topologies/topo/elements/types", rank_) ;
            var_types.setDamarisParameterAndShmem({ geomData.getNCells()}) ;

            // Copy the mesh data from the Dune grid
            long i = 0 ;
            GridDataOutput::ConnectivityVertexOrder vtkorder = GridDataOutput::VTK ;
            
            i = geomData.writeConnectivity(var_connectivity, vtkorder) ;
            if ( i  != geomData.getNCorners())
                 DUNE_THROW(Dune::IOError, geomData.getError());

            i = geomData.writeOffsetsCells(var_offsets);
            if ( i != geomData.getNCells()+1)
                 DUNE_THROW(Dune::IOError,geomData.getError());

            i = geomData.writeCellTypes(var_types) ;
            if ( i != geomData.getNCells())
                 DUNE_THROW(Dune::IOError,geomData.getError());
        }
        catch (std::exception& e) 
        {
            OpmLog::error(e.what());
        }
    }

    void prepareLocalCellData(const bool isSubStep,
                              const int  reportStepNum)
    {
        OPM_TIMEBLOCK(prepareLocalCellData);
        if (damarisOutputModule_->localDataValid()) {
            return;
        }

        const auto& gridView = simulator_.vanguard().gridView();
        const int num_interior = detail::
            countLocalInteriorCellsGridView(gridView);
        const bool log = this->collectOnIORank_.isIORank();

        damarisOutputModule_->allocBuffers(num_interior, reportStepNum,
                                      isSubStep, log, /*isRestart*/ false);

        ElementContext elemCtx(simulator_);
        OPM_BEGIN_PARALLEL_TRY_CATCH();
        {
        OPM_TIMEBLOCK(prepareCellBasedData);
        for (const auto& elem : elements(gridView, Dune::Partitions::interior)) {
            elemCtx.updatePrimaryStencil(elem);
            elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);

            damarisOutputModule_->processElement(elemCtx);
        }
        }
        if(!simulator_.model().linearizer().getFlowsInfo().empty()){
            OPM_TIMEBLOCK(prepareFlowsData);
            for (const auto& elem : elements(gridView, Dune::Partitions::interior)) {
                elemCtx.updatePrimaryStencil(elem);
                elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);
                damarisOutputModule_->processElementFlows(elemCtx);
            }
        }
        {
        OPM_TIMEBLOCK(prepareBlockData);
        for (const auto& elem : elements(gridView, Dune::Partitions::interior)) {
            elemCtx.updatePrimaryStencil(elem);
            elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);
            damarisOutputModule_->processElementBlockData(elemCtx);
        }
        }
        {
        OPM_TIMEBLOCK(prepareFluidInPlace);
#ifdef _OPENMP
#pragma omp parallel for
#endif
        for (int dofIdx=0; dofIdx < num_interior; ++dofIdx){
                const auto& intQuants = *(simulator_.model().cachedIntensiveQuantities(dofIdx, /*timeIdx=*/0));
                const auto totVolume = simulator_.model().dofTotalVolume(dofIdx);
                damarisOutputModule_->updateFluidInPlace(dofIdx, intQuants, totVolume);
        }
        }
        damarisOutputModule_->validateLocalData();
        OPM_END_PARALLEL_TRY_CATCH("DamarisWriter::prepareLocalCellData() failed: ", simulator_.vanguard().grid().comm());
    }
};

} // namespace Opm

#endif // OPM_DAMARIS_WRITER_HPP
