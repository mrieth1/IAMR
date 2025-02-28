
#include <AMReX_BC_TYPES.H>
#include <AMReX_FluxRegister.H>
#include <SyncRegister.H>
//#include <NAVIERSTOKES_F.H>
#include <SYNCREG_F.H>
#include <AMReX_BLProfiler.H>

#ifdef _OPENMP
#include <omp.h>
#endif

using namespace amrex;

SyncRegister::SyncRegister (const BoxArray& fine_boxes,
                            const DistributionMapping& dmap,
                            const IntVect&  ref_ratio)
    : ratio(ref_ratio)
{
    BL_ASSERT(grids.size() == 0);
    BL_ASSERT(fine_boxes.isDisjoint());

    grids = fine_boxes;
    grids.coarsen(ratio);

    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
	Orientation loface(dir, Orientation::low);
	Orientation hiface(dir, Orientation::high);

	BndryBATransformer lotrans(loface, IndexType::TheNodeType(), 0, 1, 0);
	BndryBATransformer hitrans(hiface, IndexType::TheNodeType(), 0, 1, 0);

	BoxArray loBA(grids, lotrans);
	BoxArray hiBA(grids, hitrans);

        bndry[loface].define(loBA,dmap,1);
        bndry_mask[loface].define(loBA,dmap,1);
        bndry[hiface].define(hiBA,dmap,1);
        bndry_mask[hiface].define(hiBA,dmap,1);
    }
}

SyncRegister::~SyncRegister () {}

void /* note that rhs is on a different BoxArray */
SyncRegister::InitRHS (MultiFab& rhs, const Geometry& geom, const BCRec& phys_bc)
{
    BL_PROFILE("SyncRegister::InitRHS()");

    BL_ASSERT(rhs.nComp() == 1);
 
    int ngrow = rhs.nGrow();

    rhs.setVal(0);

    for (OrientationIter face; face; ++face)
    {
        bndry[face()].copyTo(rhs,ngrow,0,0,bndry[face()].nComp(),geom.periodicity());
    }

    const int* phys_lo = phys_bc.lo();
    const int* phys_hi = phys_bc.hi();

    int outflow_dirs[BL_SPACEDIM-1]={-1};
    int nOutflow = 0;
    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
      if (phys_lo[dir] == Outflow || phys_hi[dir] == Outflow)
      {
	outflow_dirs[nOutflow]=dir;
	nOutflow++;
      }
    }

    const Box& node_domain = amrex::surroundingNodes(geom.Domain());

    if (nOutflow > 0)
    {      
#ifdef _OPENMP
#pragma omp parallel
#endif
      for (int n = 0; n < nOutflow; n++)
      {
	int dir = outflow_dirs[n];
	
	for (MFIter mfi(rhs); mfi.isValid(); ++mfi)
	{
	  const Box& vbx = mfi.validbox();
	  
	  if (phys_lo[dir] == Outflow)
	  {
            Box domlo(node_domain);
            domlo.setRange(dir,node_domain.smallEnd(dir),1);
	    const Box& blo = vbx & domlo;

	    if (blo.ok())
	      rhs[mfi].setVal(0.0,blo,0,1);
	  }
	  if (phys_hi[dir] == Outflow)
	  {
	    Box domhi(node_domain);
            domhi.setRange(dir,node_domain.bigEnd(dir),1);
	    const Box& bhi = vbx & domhi;

	    if (bhi.ok())
	      rhs[mfi].setVal(0.0,bhi,0,1);
	  }
	}
      }
    }

    //
    // Set Up bndry_mask.
    //
    for (OrientationIter face; face; ++face)
    {
        bndry_mask[face()].setVal(0);
    }
    
#ifdef _OPENMP
#pragma omp parallel
#endif
    {
        for (OrientationIter face_it; face_it; ++face_it)
	{
	    FabSet& fs = bndry_mask[face_it()];

	    FArrayBox tmpfab;
	    std::vector< std::pair<int,Box> > isects;	    
	    Vector<IntVect> pshifts(26);

	    for (FabSetIter fsi(fs); fsi.isValid(); ++fsi)
	    {
		FArrayBox& fab = fs[fsi];
		
		Box mask_cells = amrex::enclosedCells(amrex::grow(fab.box(),1));
		
		tmpfab.resize(mask_cells,1);
		tmpfab.setVal(0);
		
		grids.intersections(mask_cells,isects);
		
		for (int i = 0, N = isects.size(); i < N; i++)
		{
		    tmpfab.setVal(1,isects[i].second,0,1);
		}
		
		if (geom.isAnyPeriodic() && !geom.Domain().contains(mask_cells))
		{
		    geom.periodicShift(geom.Domain(),mask_cells,pshifts);
		    
		    for (Vector<IntVect>::const_iterator it = pshifts.begin(), End = pshifts.end();
			 it != End;
			 ++it)
		    {
			const IntVect& iv = *it;
			
			grids.intersections(mask_cells+iv,isects);
			
			for (int i = 0, N = isects.size(); i < N; i++)
			{
			    Box& isect = isects[i].second;
			    isect     -= iv;
			    tmpfab.setVal(1,isect,0,1);
			}
		    }
		}
		Real* mask_dat = fab.dataPtr();
		const int* mlo = fab.loVect(); 
		const int* mhi = fab.hiVect();
		Real* cell_dat = tmpfab.dataPtr();
		const int* clo = tmpfab.loVect(); 
		const int* chi = tmpfab.hiVect();
		
		makemask(mask_dat,ARLIM(mlo),ARLIM(mhi), cell_dat,ARLIM(clo),ARLIM(chi));
	    }
        }
    }

    //
    // Here double the cell contributions if at a non-periodic physical bdry.
    //
    for (int dir = 0; dir < BL_SPACEDIM; dir++)
    {
        if (!geom.isPeriodic(dir))
        {
            Box domlo(node_domain), domhi(node_domain);

            domlo.setRange(dir,node_domain.smallEnd(dir),1);
            domhi.setRange(dir,node_domain.bigEnd(dir),1);

#ifdef _OPENMP
#pragma omp parallel
#endif
            for (OrientationIter face_it; face_it; ++face_it)
            {
                FabSet& fs = bndry_mask[face_it()];

                for (FabSetIter fsi(fs); fsi.isValid(); ++fsi)
                {
                    FArrayBox& fab = fs[fsi];

                    const Box& blo = fab.box() & domlo;

                    if (blo.ok())
                        fab.mult(2.0,blo,0,1);

                    const Box& bhi = fab.box() & domhi;

                    if (bhi.ok())
                        fab.mult(2.0,bhi,0,1);
                }
            }
        }
    }
    //
    // Here convert from sum of cell contributions to 0 or 1.
    //
#ifdef _OPENMP
#pragma omp parallel
#endif
    for (OrientationIter face_it; face_it; ++face_it)
    {
        FabSet& fs = bndry_mask[face_it()];

        for (FabSetIter fsi(fs); fsi.isValid(); ++fsi)
        {
            FArrayBox& fab      = fs[fsi];
            Real*      mask_dat = fab.dataPtr();
            const int* mlo      = fab.loVect(); 
            const int* mhi      = fab.hiVect();

            convertmask(mask_dat,ARLIM(mlo),ARLIM(mhi));
        }
    }

    // Multiply by Bndry Mask

    MultiFab tmp(rhs.boxArray(), rhs.DistributionMap(), 1, ngrow);

    for (OrientationIter face; face; ++face)
    {
        BL_ASSERT(bndry_mask[face()].nComp() == 1);
	
	tmp.setVal(1.0);

	bndry_mask[face()].copyTo(tmp, ngrow, 0, 0, 1);

	MultiFab::Multiply(rhs, tmp, 0, 0, 1, ngrow);
    }
}

void
SyncRegister::CrseInit (MultiFab& Sync_resid_crse, const Geometry& crse_geom, Real mult)
{
    BL_PROFILE("SyncRegister::CrseInit()");

    setVal(0);

    Sync_resid_crse.mult(mult);

    for (OrientationIter face; face; ++face)
    {
        bndry[face()].plusFrom(Sync_resid_crse,0,0,0,1,crse_geom.periodicity());
    }
}

void
SyncRegister::CompAdd (MultiFab& Sync_resid_fine, 
                       const Geometry& fine_geom, const Geometry& crse_geom, 
		       const BoxArray& Pgrids, Real mult)
{
    BL_PROFILE("SyncRegister::CompAdd()");

#ifdef _OPENMP
#pragma omp parallel
#endif
    {
      Vector<IntVect> pshifts(27);
      std::vector< std::pair<int,Box> > isects;

      for (MFIter mfi(Sync_resid_fine); mfi.isValid(); ++mfi)
      {
	  //const Box& sync_box = mfi.tilebox();
	  const Box& sync_box = mfi.validbox();

	  Pgrids.intersections(sync_box,isects);
	  FArrayBox& syncfab = Sync_resid_fine[mfi];

	  for (int ii = 0, N = isects.size(); ii < N; ii++)
	  {
	      const int  i   = isects[ii].first;
	      const Box& pbx = Pgrids[i];

	      syncfab.setVal(0,isects[ii].second,0,1);
	      fine_geom.periodicShift(sync_box, pbx, pshifts);

	      for (Vector<IntVect>::const_iterator it = pshifts.begin(), End = pshifts.end();
		   it != End;
		   ++it)
	      {
		  Box isect = pbx + *it;
		  isect    &= sync_box;
		  syncfab.setVal(0,isect,0,1);
	      }
	  }
      }
    }

    FineAdd(Sync_resid_fine,crse_geom,mult);
}

void
SyncRegister::FineAdd (MultiFab& Sync_resid_fine, const Geometry& crse_geom, Real mult)
{
    BL_PROFILE("SyncRegister::FineAdd()");

    Sync_resid_fine.mult(mult);

    const Box& crse_node_domain = amrex::surroundingNodes(crse_geom.Domain());

    BoxArray cba = Sync_resid_fine.boxArray();
    cba.coarsen(ratio);

    MultiFab Sync_resid_crse(cba, Sync_resid_fine.DistributionMap(), 1, 0);
    Sync_resid_crse.setVal(0.0);

#ifdef _OPENMP
#pragma omp parallel
#endif
    {
	FArrayBox cbndfab;

	for (int dir = 0; dir < BL_SPACEDIM; dir++)
	{
	    for (MFIter mfi(Sync_resid_fine); mfi.isValid(); ++mfi)
	    {
		FArrayBox& finefab = Sync_resid_fine[mfi];
		FArrayBox& crsefab = Sync_resid_crse[mfi];

		const Box& finebox  = finefab.box();
		const int* resid_lo = finebox.loVect();
		const int* resid_hi = finebox.hiVect();

		const Box& crsebox  = crsefab.box();

		Box bndbox = crsebox;

		for (int side=0; side<2; ++side)
		{
		    if (side == 0) {
			bndbox.setRange(dir,crsebox.smallEnd(dir),1);
		    } else {
			bndbox.setRange(dir,crsebox.bigEnd(dir),1);
		    }

		    cbndfab.resize(bndbox, 1);
		
		    const int* clo = bndbox.loVect();
		    const int* chi = bndbox.hiVect();

		    srcrsereg(finefab.dataPtr(),
			      ARLIM(resid_lo),ARLIM(resid_hi),
			      cbndfab.dataPtr(),ARLIM(clo),ARLIM(chi),
			      clo,chi,&dir,ratio.getVect());

		    for (int j = 0; j < BL_SPACEDIM; ++j)
		    {
			if (!crse_geom.isPeriodic(j))
			{
			    //
			    // Now points on the physical bndry must be doubled
			    // for any boundary but outflow or periodic
			    //
			    Box domlo(crse_node_domain);
			    domlo.setRange(j,crse_node_domain.smallEnd(j),1);
			    domlo &= bndbox;			    
			    if (domlo.ok()) {
				cbndfab.mult(2.0,domlo,0,1);
			    }

			    Box domhi(crse_node_domain);
			    domhi.setRange(j,crse_node_domain.bigEnd(j),1);
			    domhi &= bndbox;
			    if (domhi.ok()) {
				cbndfab.mult(2.0,domhi,0,1);
			    }
			}
		    }

		    crsefab += cbndfab;
		}
            }
        }
    }

    for (OrientationIter face; face; ++face)
    {
	bndry[face()].plusFrom(Sync_resid_crse,0,0,0,1,crse_geom.periodicity());
    }
}

