/*-------------------------------------------------------------------
Copyright 2014 Ravishankar Sundararaman

This file is part of JDFTx.

JDFTx is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

JDFTx is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with JDFTx.  If not, see <http://www.gnu.org/licenses/>.
-------------------------------------------------------------------*/

#include <phonon/Phonon.h>
#include <core/Units.h>
#include <core/LatticeUtils.h>
#include <electronic/ElecMinimizer.h>
#include <electronic/operators.h>

Phonon::Phonon()
: dr(0.01), T(298*Kelvin)
{

}

inline vector3<> getCoord(const QuantumNumber& qnum) { return qnum.k; } //for k-point mapping
inline bool spinEqual(const QuantumNumber& qnum1, const QuantumNumber& qnum2) { return qnum1.spin == qnum2.spin; } //for k-point mapping (in spin polarized mode)

void Phonon::setup()
{
	//Ensure phonon command specified:
	if(!sup.length())
		die("phonon supercell must be specified using the phonon command.\n");
	if(!e.gInfo.S.length_squared())
		logPrintf(
			"WARNING: manual fftbox setting recommended for phonon. If supercell\n"
			"     grid initialization fails below, specify larger manual fftbox.\n");
	//Check symmetries
	if(e.symm.mode != SymmetriesNone)
		die("phonon does not support symmetries.\n");
	//Check kpoint and supercell compatibility:
	if(e.eInfo.qnums.size()>1 || e.eInfo.qnums[0].k.length_squared())
		die("phonon requires a Gamma-centered uniform kpoint mesh.\n");
	for(int j=0; j<3; j++)
	{	if(e.eInfo.kfold[j] % sup[j])
		{	die("kpoint folding %d is not a multiple of supercell count %d for lattice direction %d.\n",
				e.eInfo.kfold[j], sup[j], j);
		}
		eSup.eInfo.kfold[j] = e.eInfo.kfold[j] / sup[j];
	}
	
	logPrintf("########### Initializing JDFTx for unit cell #############\n");
	SpeciesInfo::Constraint constraintFull;
	constraintFull.moveScale = 0;
	constraintFull.type = SpeciesInfo::Constraint::None;
	for(size_t sp=0; sp<e.iInfo.species.size(); sp++)
		e.iInfo.species[sp]->constraints.assign(e.iInfo.species[sp]->atpos.size(), constraintFull);
	e.setup();
	
	logPrintf("########### Initializing JDFTx for supercell #############\n");
	//Grid:
	eSup.gInfo.S = Diag(sup) * e.gInfo.S; //ensure exact supercell
	eSup.gInfo.R = e.gInfo.R * Diag(sup);
	int prodSup = sup[0] * sup[1] * sup[2];
	//Replicate atoms:
	for(size_t sp=0; sp<e.iInfo.species.size(); sp++)
	{	eSup.iInfo.species[sp]->atpos.clear();
		eSup.iInfo.species[sp]->initialMagneticMoments.clear();
		matrix3<> invSup = inv(Diag(vector3<>(sup)));
		vector3<int> iR;
		for(iR[0]=0; iR[0]<sup[0]; iR[0]++)
		for(iR[1]=0; iR[1]<sup[1]; iR[1]++)
		for(iR[2]=0; iR[2]<sup[2]; iR[2]++)
			for(vector3<> pos: e.iInfo.species[sp]->atpos)
				eSup.iInfo.species[sp]->atpos.push_back(invSup * (pos + iR));
		eSup.iInfo.species[sp]->constraints.assign(eSup.iInfo.species[sp]->atpos.size(), constraintFull);
	}
	//Remove initial state settings (incompatible with supercell):
	eSup.eVars.wfnsFilename.clear();
	eSup.eVars.HauxFilename.clear();
	eSup.eVars.eigsFilename.clear();
	eSup.eVars.fluidInitialStateFilename.clear();
	eSup.eInfo.initialFillingsFilename.clear();
	eSup.scfParams.historyFilename.clear();
	//ElecInfo:
	eSup.eInfo.nBands = e.eInfo.nBands * prodSup;
	//ElecVars:
	eSup.eVars.initLCAO = false;
	eSup.setup();
	
	//Map states:
	PeriodicLookup<QuantumNumber> plook(eSup.eInfo.qnums, eSup.gInfo.GGT);
	std::vector<int> nqPrev(eSup.eInfo.qnums.size(), 0);
	for(int q=0; q<e.eInfo.nStates; q++)
	{	const QuantumNumber& qnum = e.eInfo.qnums[q];
		vector3<> kSup = matrix3<>(Diag(sup)) * qnum.k; //qnum.k in supercell reciprocal lattice coords
		size_t qSup = plook.find(kSup, qnum, &(eSup.eInfo.qnums), spinEqual);
		assert(qSup != string::npos);
		vector3<> iGtmp = kSup - eSup.eInfo.qnums[qSup].k;
		//Add to stateMap:
		auto sme = std::make_shared<StateMapEntry>();
		sme->qSup = qSup;
		for(int j=0; j<3; j++)
			sme->iG[j] = round(iGtmp[j]);
		sme->nqPrev = nqPrev[qSup];
		nqPrev[qSup]++;
		stateMap.push_back(sme);
	}
	for(int nq: nqPrev) assert(nq == prodSup); //each supercell k-point must be mapped to prod(sup) unit cell kpoints
	
	//Map corresponding basis objects:
	for(int qSup=eSup.eInfo.qStart; qSup<eSup.eInfo.qStop; qSup++)
	{	//Create lookup table for supercell indices:
		vector3<int> iGbox; //see Basis::setup(), this will bound the values of iG in the basis
		for(int i=0; i<3; i++)
			iGbox[i] = 1 + int(sqrt(2*eSup.cntrl.Ecut) * eSup.gInfo.R.column(i).length() / (2*M_PI));
		vector3<int> pitch;
		pitch[2] = 1;
		pitch[1] = pitch[2] * (2*iGbox[2]+1);
		pitch[0] = pitch[1] * (2*iGbox[1]+1);
		std::vector<int> supIndex(pitch[0] * (2*iGbox[0]+1), -1);
		const Basis& basisSup = eSup.basis[qSup];
		for(size_t n=0; n<basisSup.nbasis; n++)
			supIndex[dot(pitch,basisSup.iGarr[n]+iGbox)] = n;
		//Initialize index map for each unit cell k-point
		for(int q=0; q<e.eInfo.nStates; q++) if(stateMap[q]->qSup == qSup)
		{	const Basis& basis = e.basis[q];
			std::vector<int> indexVec(basis.nbasis);
			for(size_t n=0; n<basis.nbasis; n++)
			{	vector3<int> iGsup = basis.iGarr[n];
				for(int j=0; j<3; j++) iGsup[j] *= sup[j]; //to supercell reciprocal lattice coords
				iGsup += stateMap[q]->iG; //offset due to Bloch phase
				indexVec[n] = supIndex[dot(pitch,iGsup+iGbox)]; //lookup from above table
			}
			assert(*std::min_element(indexVec.begin(), indexVec.end()) >= 0); //make sure all entries were found
			stateMap[q]->setIndex(indexVec);
		}
	}
}

void Phonon::dump()
{
	//Initialize state of unit cell:
	logPrintf("########### Unperturbed unit cell calculation #############\n");
	if(e.cntrl.dumpOnly)
	{	//Single energy calculation so that all dependent quantities have been initialized:
		logPrintf("\n----------- Energy evaluation at fixed state -------------\n"); logFlush();
		e.eVars.elecEnergyAndGrad(e.ener, 0, 0, true);
	}
	else elecFluidMinimize(e);
	logPrintf("# Energy components:\n"); e.ener.print(); logPrintf("\n");

	//Make unit cell state available on all processes 
	//(since MPI division of qSup and q are different and independent of the map)
	for(int q=0; q<e.eInfo.nStates; q++)
	{	//Allocate:
		if(!e.eInfo.isMine(q))
		{	e.eVars.C[q].init(e.eInfo.nBands, e.basis[q].nbasis * e.eInfo.spinorLength(), &e.basis[q], &e.eInfo.qnums[q]);
			e.eVars.F[q].resize(e.eInfo.nBands);
			if(e.eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
				e.eVars.B[q].init(e.eInfo.nBands, e.eInfo.nBands);
		}
		//Broadcast from owner:
		int qSrc = e.eInfo.whose(q);
		e.eVars.C[q].bcast(qSrc);
		e.eVars.F[q].bcast(qSrc);
		if(e.eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
			e.eVars.B[q].bcast(qSrc);
	}
	
	//List of displacements:
	struct Mode { int sp; int at; int dir; }; //specie, atom and cartesian directions for each displacement
	std::vector<Mode> modes;
	for(size_t sp=0; sp<e.iInfo.species.size(); sp++)
		for(size_t at=0; at<e.iInfo.species[sp]->atpos.size(); at++) //only need to move atoms in first unit cell
			for(int dir=0; dir<3; dir++)
			{	Mode mode = { int(sp), int(at), dir };
				modes.push_back(mode);
			}
	
	//Initialize state of supercell:
	logPrintf("########### Unperturbed supercell calculation #############\n");
	setSupState();
	IonicMinimizer imin(eSup);
	IonicGradient grad0;
	imin.compute(&grad0); //compute initial forces and energy
	logPrintf("# Energy components:\n"); eSup.ener.print(); logPrintf("\n");
	int prodSup = sup[0]*sup[1]*sup[2];
	double E0 = relevantFreeEnergy(eSup);
	logPrintf("Supercell energy discrepancy: %lg / unit cell\n", E0/prodSup - relevantFreeEnergy(e));
	logPrintf("RMS force in initial configuration: %lg\n", sqrt(dot(grad0,grad0)/(modes.size()*prodSup)));
	//subspace Hamiltonian of supercell Gamma point:
	setSupState();
	std::vector<matrix> Hsub0 = getHsub();
	
	//Displaced modes:
	std::vector<IonicGradient> dgrad(modes.size()); //change in gradient for each perturbation
	std::vector< std::vector<matrix> > dHsub(modes.size()); //chaneg in Hsub for each perturbation
	for(size_t iMode=0; iMode<modes.size(); iMode++)
	{	const Mode& mode = modes[iMode];
		logPrintf("\n########### Perturbed supercell calculation %d of %d #############\n", int(iMode+1), int(modes.size()));
		//Move one atom:
		IonicGradient dir; dir.init(eSup.iInfo); //contains zeroes
		dir[mode.sp][mode.at][mode.dir] = 1.;
		imin.step(dir, dr);
		//Calculate energy and forces:
		IonicGradient grad;
		imin.compute(&grad);
		dgrad[iMode] = (grad - grad0) * (1./dr);
		logPrintf("Energy change: %lg / unit cell\n", (relevantFreeEnergy(eSup) - E0)/prodSup);
		logPrintf("RMS force: %lg\n", sqrt(dot(grad,grad)/(modes.size()*prodSup)));
		//Subspace hamiltonian change:
		setSupState(); //for e-ph matrix elements, need overlap of original states against perturbation Hamiltonian
		std::vector<matrix> Hsub = getHsub();
		dHsub[iMode].resize(Hsub.size());
		for(size_t s=0; s<Hsub.size(); s++)
			dHsub[iMode][s] = (1./dr) * (Hsub[s] - Hsub0[s]);
		//Restore atom position:
		bool dragWfns = false;
		std::swap(dragWfns, eSup.cntrl.dragWavefunctions); //disable wave function drag because state has already been restored to unperturbed version
		imin.step(dir, -dr);
		std::swap(dragWfns, eSup.cntrl.dragWavefunctions); //restore wave function drag flag
	}
	
	//Construct frequency-squared matrix:
	//--- convert forces to frequency-squared (divide by mass symmetrically):
	std::vector<double> invsqrtM;
	for(auto sp: e.iInfo.species)
		invsqrtM.push_back(1./sqrt(sp->mass * amu));
	for(size_t iMode=0; iMode<modes.size(); iMode++)
	{	dgrad[iMode] *= invsqrtM[modes[iMode].sp]; //divide by sqrt(M) on the left
		for(size_t sp=0; sp<e.iInfo.species.size(); sp++)
			for(vector3<>& f: dgrad[iMode][sp])
				f *= invsqrtM[sp]; // divide by sqrt(M) on the right
	}
	//--- remap to cells:
	std::map<vector3<int>,double> cellMap = getCellMap(e.gInfo.R, eSup.gInfo.R, e.dump.getFilename("phononCellMap"));
	std::vector<matrix> omegaSq(cellMap.size());
	auto iter = cellMap.begin();
	for(size_t iCell=0; iCell<cellMap.size(); iCell++)
	{	//Get cell map entry:
		vector3<int> iR = iter->first;
		double weight = iter->second;
		iter++;
		//Find index of cell in supercell:
		for(int j=0; j<3; j++)
			iR[j] = positiveRemainder(iR[j], sup[j]);
		int cellIndex =  (iR[0]*sup[1] + iR[1])*sup[2] + iR[2]; //corresponding to the order of atom replication in setup()
		//Collect omegSq entries:
		omegaSq[iCell].init(modes.size(), modes.size());
		for(size_t iMode1=0; iMode1<modes.size(); iMode1++)
		{	const IonicGradient& F1 = dgrad[iMode1];
			for(size_t iMode2=0; iMode2<modes.size(); iMode2++)
			{	const Mode& mode2 = modes[iMode2];
				size_t cellOffsetSp = cellIndex * e.iInfo.species[mode2.sp]->atpos.size(); //offset into atoms of current cell for current species
				omegaSq[iCell].set(iMode1,iMode2, weight * F1[mode2.sp][mode2.at + cellOffsetSp][mode2.dir]);
			}
		}
	}
	//--- enforce hermiticity:
	size_t nSymmetrizedCells = 0;
	auto iter1 = cellMap.begin();
	for(size_t iCell1=0; iCell1<cellMap.size(); iCell1++)
	{	auto iter2 = cellMap.begin();
		for(size_t iCell2=0; iCell2<cellMap.size(); iCell2++)
		{	vector3<int> iRsum = iter1->first + iter2->first;
			if(!iRsum.length_squared() && iCell2>=iCell1) //loop over iR1 + iR2 == 0 pairs
			{	matrix M = 0.5*(omegaSq[iCell1] + dagger(omegaSq[iCell2]));
				omegaSq[iCell1] = M;
				omegaSq[iCell2] = dagger(M);
				nSymmetrizedCells += (iCell1==iCell2 ? 1 : 2);
			}
			iter2++;
		}
		iter1++;
	}
	assert(nSymmetrizedCells == cellMap.size());
	//--- enforce translational invariance:
	matrix omegaSqSum;
	for(const matrix& M: omegaSq)
		omegaSqSum += M;
	matrix Fmean; //3x3 force matrix for all atoms moving together at Gamma point (should be zero)
	int nAtoms = modes.size()/3;
	for(int at=0; at<nAtoms; at++)
		Fmean += (1./(nAtoms*prodSup)) * omegaSqSum(3*at,3*(at+1), 3*at,3*(at+1)) * e.iInfo.species[modes[3*at].sp]->mass;
	matrix omegaSqCorrection = zeroes(modes.size(), modes.size());
	for(int at=0; at<nAtoms; at++)
		omegaSqCorrection.set(3*at,3*(at+1), 3*at,3*(at+1), Fmean * (1./e.iInfo.species[modes[3*at].sp]->mass));
	iter = cellMap.begin();
	for(size_t iCell=0; iCell<cellMap.size(); iCell++)
	{	omegaSq[iCell] -= iter->second * omegaSqCorrection;
		iter++;
	}
	//--- write to file
	if(mpiUtil->isHead())
	{	string fname = e.dump.getFilename("phononOmegaSq");
		logPrintf("Writing '%s' ... ", fname.c_str()); logFlush();
		FILE* fp = fopen(fname.c_str(), "w");
		for(const matrix& M: omegaSq)
			M.write_real(fp); //M is explicitly real by construction above
		fclose(fp);
		logPrintf("done.\n"); logFlush();
		//Write description of modes:
		fname = e.dump.getFilename("phononBasis");
		logPrintf("Writing '%s' ... ", fname.c_str()); logFlush();
		fp = fopen(fname.c_str(), "w");
		fprintf(fp, "#species atom dx dy dz [bohrs]");
		for(const Mode& mode: modes)
		{	vector3<> r; r[mode.dir] = invsqrtM[mode.sp];
			fprintf(fp, "%s %d %lg %lg %lg\n", e.iInfo.species[mode.sp]->name.c_str(), mode.at, r[0], r[1], r[2]);
		}
		fclose(fp);
		logPrintf("done.\n"); logFlush();
	}
}

void Phonon::setSupState()
{
	//Zero wavefunctions and auxiliary Hamiltonia (since a scatter used below)
	for(int qSup=eSup.eInfo.qStart; qSup<eSup.eInfo.qStop; qSup++)
	{	eSup.eVars.C[qSup].zero();
		if(e.eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
			eSup.eVars.B[qSup].zero();
	}
	
	//Update supercell quantities:
	int nSpinor = e.eInfo.spinorLength();
	double scaleFac = 1./sqrt(sup[0]*sup[1]*sup[2]); //to account for normalization
	for(int q=0; q<e.eInfo.nStates; q++) if(eSup.eInfo.isMine(stateMap[q]->qSup))
	{	int nBandsPrev = e.eInfo.nBands * stateMap[q]->nqPrev;
		int qSup = stateMap[q]->qSup;
		//Wavefunctions:
		const ColumnBundle& C = e.eVars.C[q];
		ColumnBundle& Csup = eSup.eVars.C[qSup];
		for(int b=0; b<e.eInfo.nBands; b++)
			for(int s=0; s<nSpinor; s++)
				callPref(eblas_scatter_zdaxpy)(stateMap[q]->nIndices, scaleFac, stateMap[q]->indexPref,
					C.dataPref() + C.index(b,s*C.basis->nbasis),
					Csup.dataPref() + Csup.index(b + nBandsPrev, s*Csup.basis->nbasis));
		//Fillings:
		const diagMatrix& F = e.eVars.F[q];
		diagMatrix& Fsup = eSup.eVars.F[qSup];
		Fsup.set(nBandsPrev,nBandsPrev+e.eInfo.nBands, F);
		//Auxiliary Hamiltonian (if necessary):
		if(e.eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
		{	const matrix& B = e.eVars.B[q];
			matrix& Bsup = eSup.eVars.B[qSup];
			Bsup.set(nBandsPrev,nBandsPrev+e.eInfo.nBands, nBandsPrev,nBandsPrev+e.eInfo.nBands, B);
		}
	}
	
	//Update wave function indep variables (Y) and projections
	for(int qSup=eSup.eInfo.qStart; qSup<eSup.eInfo.qStop; qSup++)
	{	eSup.eVars.Y[qSup] = eSup.eVars.C[qSup];
		eSup.iInfo.project(eSup.eVars.C[qSup], eSup.eVars.VdagC[qSup]);
	}
	
	//Update entropy contributions:
	if(eSup.eInfo.fillingsUpdate != ElecInfo::ConstantFillings)
		eSup.eInfo.updateFillingsEnergies(eSup.eVars.F, eSup.ener);
	
	if(e.eInfo.fillingsUpdate==ElecInfo::FermiFillingsAux)
		eSup.eVars.HauxInitialized = true;
}

std::vector<matrix> Phonon::getHsub()
{	int nSpins = e.eInfo.spinType==SpinZ ? 2 : 1;
	std::vector<matrix> Hsub(nSpins);
	for(int s=0; s<nSpins; s++)
	{	int qSup = s*(eSup.eInfo.nStates/nSpins); //Gamma point is always first in the list for each spin
		assert(eSup.eInfo.qnums[qSup].k.length_squared() == 0); //make sure that above is true
		if(eSup.eInfo.isMine(qSup))
		{	ColumnBundle HC; Energies ener;
			eSup.eVars.applyHamiltonian(qSup, eye(eSup.eInfo.nBands), HC, ener, true);
			Hsub[s] = eSup.eVars.Hsub[qSup];
		}
		else Hsub[s].init(eSup.eInfo.nBands, eSup.eInfo.nBands);
		Hsub[s].bcast(eSup.eInfo.whose(qSup));
	}
	return Hsub;
}

Phonon::StateMapEntry::StateMapEntry() : nIndices(0), indexPref(0), index(0)
#ifdef GPU_ENABLED
, indexGpu(0)
#endif
{

}

Phonon::StateMapEntry::~StateMapEntry()
{
	if(index) delete[] index;
	#ifdef GPU_ENABLED
	if(indexGpu) cudaFree(indexGpu);
	#endif
}

void Phonon::StateMapEntry::setIndex(const std::vector<int>& indexVec)
{	nIndices = indexVec.size();
	//CPU version
	if(index) delete[] index;
	index = new int[nIndices];
	eblas_copy(index, indexVec.data(), nIndices);
	indexPref = index;
	//GPU version
	#ifdef GPU_ENABLED
	if(indexGpu) cudaFree(indexGpu);
	cudaMalloc(&indexGpu, sizeof(int)*nIndices); gpuErrorCheck();
	cudaMemcpy(indexGpu, index, sizeof(int)*nIndices, cudaMemcpyHostToDevice); gpuErrorCheck();
	indexPref = indexGpu;
	#endif
}