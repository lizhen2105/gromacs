#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "typedefs.h"
#include "smalloc.h"
#include "vec.h"
#include "domdec.h"
#include "nrnb.h"
#include "pbc.h"
#include "constr.h"
#include "mdatoms.h"
#include "names.h"
#include "pdbio.h"
#include "futil.h"
#include "pme.h"
#include "pull.h"
#include "gmx_wallcycle.h"
#include "mdrun.h"
#include "nsgrid.h"
#include "shellfc.h"
#include "perf_est.h"

#ifdef GMX_MPI
#include <mpi.h>
#endif

#define DDRANK(dd,rank)    (rank)
#define DDMASTERRANK(dd)   (dd->masterrank)

typedef struct gmx_domdec_master {
  /* The cell boundaries */
  real **cell_x;
  /* The global charge group division */
  int  *ncg;     /* Number of home charge groups for each node */
  int  *index;   /* Index of nnodes+1 into cg */
  int  *cg;      /* Global charge group index */
  int  *nat;     /* Number of home atoms for each node. */
  int  *ibuf;    /* Buffer for communication */
} gmx_domdec_master_t;

typedef struct {
  /* The numbers of charge groups to send and receive for each cell
   * that requires communication, the last entry contains the total
   * number of atoms that needs to be communicated.
   */
  int nsend[DD_MAXICELL+2];
  int nrecv[DD_MAXICELL+2];
  /* The charge groups to send */
  int *index;
  int nalloc;
  /* The atom range for non-in-place communication */
  int cell2at0[DD_MAXICELL];
  int cell2at1[DD_MAXICELL];
} gmx_domdec_ind_t;

typedef struct {
  int  np;                     /* Number of grid pulses in this dimension */
  gmx_domdec_ind_t *ind;       /* The indices to communicate, size np     */
  int  np_nalloc;
  bool bInPlace;               /* Can we communicate in place?            */
} gmx_domdec_comm_dim_t;

typedef struct {
  real *cell_size;
  bool *bCellMin;
  real *cell_f;
  real *old_cell_f;
  real *cell_f_max0;
  real *cell_f_min1;
  real *bound_min;
  real *bound_max;
  bool bLimited;
} gmx_domdec_root_t;

#define DD_NLOAD_MAX 9

/* Here floats are accurate enough, since these variables
 * only influence the load balancing, not the actual MD results.
 */
typedef struct {
  int  nload;
  float *load;
  float sum;
  float max;
  float sum_m;
  float cvol_min;
  float mdf;
  float pme;
  int   flags;
} gmx_domdec_load_t;

typedef struct {
  int  nsc;
  int  ind_gl;
  int  ind;
} gmx_cgsort_t;

typedef struct {
  gmx_cgsort_t *sort1,*sort2;
  int  sort_nalloc;
  gmx_cgsort_t *sort_new;
  int  sort_new_nalloc;
  rvec *vbuf;
  int  vbuf_nalloc;
  int  *ibuf;
  int  ibuf_nalloc;
} gmx_domdec_sort_t;

/* This enum determines the order of the coordinates.
 * ddnatHOME and ddnatZONE should be first and second,
 * the others can be ordered as wanted.
 */
enum { ddnatHOME, ddnatZONE, ddnatVSITE, ddnatCON, ddnatNR };

typedef struct gmx_domdec_comm {
  /* All arrays are indexed with 0 to dd->ndim (not Cartesian indexing),
   * unless stated otherwise.
   */

  /* The number of nodes doing PME (PP/PME or only PME) */
  int  npmenodes;
  /* The communication setup including the PME only nodes */
  bool bCartesianPP_PME;
  ivec ntot;
  int  cartpmedim;
  int  *pmenodes;          /* size npmenodes                         */
  int  *ddindex2simnodeid; /* size npmenodes, only with bCartesianPP
			    * but with bCartesianPP_PME              */
  int  *pme_xmin;          /* size npmenodes                         */
  int  *pme_xmax;          /* size npmenodes                         */
  int  pme_maxshift;

  /* The DD particle-particle nodes only */
  /* The communication setup within the communicator all */
#ifdef GMX_MPI
  MPI_Comm all;
#endif
  bool bCartesianPP;
  int  *ddindex2ddnodeid; /* size npmenode, only with bCartesianPP_PME */

  /* How to communicate for constraints and vsites */
  bool bSendRecv2;

  /* Should we sort the cgs */
  int  nstSortCG;
  gmx_domdec_sort_t *sort;
  bool bFilled_nsgrid_home;

  /* Are there bonded and multi-body interactions between charge groups? */
  bool bInterCGBondeds;
  bool bInterCGMultiBody;

  /* Cell sizes for static load balancing, first index cartesian */
  real **slb_frac;
  /* Cell sizes for determining the PME communication with SLB */
  real *pme_dim_f;
    
  /* The width of the communicated boundaries */
  real cutoff_mbody;
  real cutoff;
  /* The minimum cell size (including triclinic correction) */
  rvec cellsize_min;
  /* The lower limit for the DD cell size with DLB */
  real cellsize_limit;

  /* Orthogonal vectors for triclinic cells, Cartesian index */
  rvec v[DIM][DIM];

  /* The old location of the cell boundaries, to check cg displacements */
  rvec old_cell_x0;
  rvec old_cell_x1;

  /* The cell boundaries of neighboring cells for dynamic load balancing */
  real **cell_d1;
  real ***cell_d2;

  /* The coordinate/force communication setup and indices */
  gmx_domdec_comm_dim_t cd[DIM];
  /* The maximum number of cells to communicate with in one dimension */
  int  maxpulse;

  /* The number of cg's received from the direct neighbors */
  int  cell_ncg1[DD_MAXCELL];

  /* The atom counts, the range for each type t is nat[t-1] <= at < nat[t] */
  int  nat[ddnatNR];

  /* Communication buffer for general use */
  int  *buf_int;
  int  nalloc_int;

  /* Communication buffers only used with multiple grid pulses */
  int  *buf_int2;
  int  nalloc_int2;
  rvec *buf_vr2;
  int  nalloc_vr2;

  /* Communication buffers for local redistribution */
  int  **cggl_flag;
  int  cggl_flag_nalloc[DIM*2];
  rvec **cgcm_state;
  int  cgcm_state_nalloc[DIM*2];
  rvec *buf_vr;
  int  nalloc_vr;

  /* Cell sizes for dynamic load balancing */
  gmx_domdec_root_t **root;
  real *cell_f_row;
  real cell_f0[DIM];
  real cell_f1[DIM];
  real cell_f_max0[DIM];
  real cell_f_min1[DIM];

  /* Stuff for load communication */
  bool bRecordLoad;
  gmx_domdec_load_t *load;
#ifdef GMX_MPI
  MPI_Comm *mpi_comm_load;
#endif
  /* Cycle counters */
  float cycl[ddCyclNr];
  int   cycl_n[ddCyclNr];
  /* Flop counter (0=no,1=yes,2=with (eFlop-1)*5% noise */
  int eFlop;
  double flop;
  int    flop_n;
  /* Have we measured the load? */
  bool bHaveLoad;
  /* Have we printed the load at least once? */
  bool bFirstPrinted;

  /* Statistics */
  double sum_nat[ddnatNR-ddnatZONE];
  int    ndecomp;
  int    nload;
  double load_step;
  double load_sum;
  double load_max;
  ivec   load_lim;
  double load_mdf;
  double load_pme;
} gmx_domdec_comm_t;

/* The size per charge group of the cggl_flag buffer in gmx_domdec_comm_t */
#define DD_CGIBS 2

/* The flags for the cggl_flag buffer in gmx_domdec_comm_t */
#define DD_FLAG_NRCG  65535
#define DD_FLAG_FW(d) (1<<(16+(d)*2))
#define DD_FLAG_BW(d) (1<<(16+(d)*2+1))

/* Cell permutation required to obtain consecutive charge groups
 * for neighbor searching.
 */
static const int cell_perm[3][4] = { {0,0,0,0},{1,0,0,0},{3,0,1,2} };

/* The DD cell order */
static const ivec dd_co[DD_MAXCELL] =
  {{0,0,0},{1,0,0},{1,1,0},{0,1,0},{0,1,1},{0,0,1},{1,0,1},{1,1,1}};

/* The 3D setup */
#define dd_c3n  8
#define dd_cp3n 4
static const ivec dd_cp3[dd_cp3n] = {{0,0,8},{1,3,6},{2,5,6},{3,5,7}};

/* The 2D setup */
#define dd_c2n  4
#define dd_cp2n 2
static const ivec dd_cp2[dd_cp2n] = {{0,0,4},{1,3,4}};

/* The 1D setup */
#define dd_c1n  2
#define dd_cp1n 1
static const ivec dd_cp1[dd_cp1n] = {{0,0,2}};

static int nstDDDump,nstDDDumpGrid;

/* Factors used to avoid problems due to rounding issues */
#define DD_CELL_MARGIN       1.00001
#define DD_CELL_MARGIN2      1.000005
/* Factor to account for pressure scaling during nstlist steps */
#define DD_PRES_SCALE_MARGIN 1.02

/* Margin for setting up the DD grid */
#define DD_GRID_MARGIN_PRES_SCALE 1.05

#define DD_CELL_F_SIZE(dd,di) ((dd)->nc[(dd)->dim[(di)]]+1+(di)*2+1)


/*
#define dd_index(n,i) ((((i)[ZZ]*(n)[YY] + (i)[YY])*(n)[XX]) + (i)[XX])

static void index2xyz(ivec nc,int ind,ivec xyz)
{
  xyz[XX] = ind % nc[XX];
  xyz[YY] = (ind / nc[XX]) % nc[YY];
  xyz[ZZ] = ind / (nc[YY]*nc[XX]);
}
*/

/* This order is required to minimize the coordinate communication in PME
 * which uses decomposition in the x direction.
 */
#define dd_index(n,i) ((((i)[XX]*(n)[YY] + (i)[YY])*(n)[ZZ]) + (i)[ZZ])

static void ddindex2xyz(ivec nc,int ind,ivec xyz)
{
  xyz[XX] = ind / (nc[YY]*nc[ZZ]);
  xyz[YY] = (ind / nc[ZZ]) % nc[YY];
  xyz[ZZ] = ind % nc[ZZ];
}

static int ddcoord2ddnodeid(gmx_domdec_t *dd,ivec c)
{
  int ddindex;
  int ddnodeid=-1;

  ddindex = dd_index(dd->nc,c);
  if (dd->comm->bCartesianPP_PME) {
    ddnodeid = dd->comm->ddindex2ddnodeid[ddindex];
  } else if (dd->comm->bCartesianPP) {
#ifdef GMX_MPI
    MPI_Cart_rank(dd->comm->all,c,&ddnodeid);
#endif
  } else {
    ddnodeid = ddindex;
  }

  return ddnodeid;
}

int glatnr(gmx_domdec_t *dd,int i)
{
  int atnr;

  if (dd == NULL) {
    atnr = i + 1;
  } else {
    if (i >= dd->comm->nat[ddnatNR-1])
      gmx_fatal(FARGS,"glatnr called with %d, which is larger than the local number of atoms (%d)",i,dd->comm->nat[ddnatNR-1]);
    atnr = dd->gatindex[i] + 1;
  }

  return atnr;
}

bool dd_filled_nsgrid_home(gmx_domdec_t *dd)
{
  return dd->comm->bFilled_nsgrid_home;
}

void dd_store_state(gmx_domdec_t *dd,t_state *state)
{
  int i;

  if (state->ddp_count != dd->ddp_count)
    gmx_incons("The state does not the domain decomposition state");

  state->ncg_gl = dd->ncg_home;
  if (state->ncg_gl > state->cg_gl_nalloc) {
    state->cg_gl_nalloc = over_alloc_dd(state->ncg_gl);
    srenew(state->cg_gl,state->cg_gl_nalloc);
  }
  for(i=0; i<state->ncg_gl; i++)
    state->cg_gl[i] = dd->index_gl[i];
  
  state->ddp_count_cg_gl = dd->ddp_count;
}

void dd_get_ns_ranges(gmx_domdec_t *dd,int icg,
		      int *jcg0,int *jcg1,ivec shift0,ivec shift1)
{
  int icell,d,dim;

  icell = 0;
  while (icg >= dd->icell[icell].cg1)
    icell++;

  if (icell == 0)
    *jcg0 = icg;
  else if (icell < dd->nicell)
    *jcg0 = dd->icell[icell].jcg0;
  else
    gmx_fatal(FARGS,"DD icg %d out of range: icell (%d) >= nicell (%d)",
	      icg,icell,dd->nicell);

  *jcg1 = dd->icell[icell].jcg1;

  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    shift0[dim] = dd->icell[icell].shift0[dim];
    shift1[dim] = dd->icell[icell].shift1[dim];
    if (dd->tric_dir[dim] || (dd->bGridJump && d > 0)) {
      /* A conservative approach, this can be optimized */
      shift0[dim] -= 1;
      shift1[dim] += 1;
    }
  }
}

int dd_natoms_vsite(gmx_domdec_t *dd)
{
  return dd->comm->nat[ddnatVSITE];
}

void dd_get_constraint_range(gmx_domdec_t *dd,int *at_start,int *at_end)
{
  *at_start = dd->comm->nat[ddnatCON-1];
  *at_end   = dd->comm->nat[ddnatCON];
}

void dd_sendrecv_int(const gmx_domdec_t *dd,
		     int ddim,int direction,
		     int *buf_s,int n_s,
		     int *buf_r,int n_r)
{
#ifdef GMX_MPI
  int rank_s,rank_r;
  MPI_Status stat;

  rank_s = dd->neighbor[ddim][direction==ddForward ? 0 : 1];
  rank_r = dd->neighbor[ddim][direction==ddForward ? 1 : 0];

  if (n_s && n_r) {
    MPI_Sendrecv(buf_s,n_s*sizeof(int),MPI_BYTE,rank_s,0,
		 buf_r,n_r*sizeof(int),MPI_BYTE,rank_r,0,
		 dd->comm->all,&stat);
  } else if (n_s) {
    MPI_Send(    buf_s,n_s*sizeof(int),MPI_BYTE,rank_s,0,
		 dd->comm->all);
  } else if (n_r) {
    MPI_Recv(    buf_r,n_r*sizeof(int),MPI_BYTE,rank_r,0,
	         dd->comm->all,&stat);
  }
    
#endif
}

void dd_sendrecv_rvec(const gmx_domdec_t *dd,
		      int ddim,int direction,
		      rvec *buf_s,int n_s,
		      rvec *buf_r,int n_r)
{
#ifdef GMX_MPI
  int rank_s,rank_r;
  MPI_Status stat;

  rank_s = dd->neighbor[ddim][direction==ddForward ? 0 : 1];
  rank_r = dd->neighbor[ddim][direction==ddForward ? 1 : 0];

  if (n_s && n_r) {
    MPI_Sendrecv(buf_s[0],n_s*sizeof(rvec),MPI_BYTE,rank_s,0,
		 buf_r[0],n_r*sizeof(rvec),MPI_BYTE,rank_r,0,
		 dd->comm->all,&stat);
  } else if (n_s) {
    MPI_Send(    buf_s[0],n_s*sizeof(rvec),MPI_BYTE,rank_s,0,
		 dd->comm->all);
  } else if (n_r) {
    MPI_Recv(    buf_r[0],n_r*sizeof(rvec),MPI_BYTE,rank_r,0,
	         dd->comm->all,&stat);
  }

#endif
}

void dd_sendrecv2_rvec(const gmx_domdec_t *dd,
		       int ddim,
		       rvec *buf_s_fw,int n_s_fw,
		       rvec *buf_r_fw,int n_r_fw,
		       rvec *buf_s_bw,int n_s_bw,
		       rvec *buf_r_bw,int n_r_bw)
{
#ifdef GMX_MPI
  int rank_fw,rank_bw,nreq;
  MPI_Request req[4];
  MPI_Status  stat[4];

  rank_fw = dd->neighbor[ddim][0];
  rank_bw = dd->neighbor[ddim][1];

  if (!dd->comm->bSendRecv2) {
    /* Try to send and receive in two directions simultaneously.
     * Should be faster, especially on machines
     * with full 3D communication networks.
     * However, it could be that communication libraries are
     * optimized for MPI_Sendrecv and non-blocking MPI calls
     * are slower.
     * SendRecv2 can be turned on with the env.var. GMX_DD_SENDRECV2
     */
    nreq = 0;
    if (n_r_fw)
      MPI_Irecv(buf_r_fw[0],n_r_fw*sizeof(rvec),MPI_BYTE,
		rank_bw,0,dd->comm->all,&req[nreq++]);
    if (n_r_bw)
      MPI_Irecv(buf_r_bw[0],n_r_bw*sizeof(rvec),MPI_BYTE,
		rank_fw,1,dd->comm->all,&req[nreq++]);
    if (n_s_fw)
      MPI_Isend(buf_s_fw[0],n_s_fw*sizeof(rvec),MPI_BYTE,
		rank_fw,0,dd->comm->all,&req[nreq++]);
    if (n_s_bw)
      MPI_Isend(buf_s_bw[0],n_s_bw*sizeof(rvec),MPI_BYTE,
		rank_bw,1,dd->comm->all,&req[nreq++]);
    if (nreq)
      MPI_Waitall(nreq,req,stat);
  } else {
    /* Communicate in two ordered phases.
     * This is slower, even on a dual-core Opteron cluster
     * with a single full-duplex network connection per machine.
     */
    /* Forward */
    MPI_Sendrecv(buf_s_fw[0],n_s_fw*sizeof(rvec),MPI_BYTE,rank_fw,0,
		 buf_r_fw[0],n_r_fw*sizeof(rvec),MPI_BYTE,rank_bw,0,
		 dd->comm->all,&stat[0]);
    /* Backward */
    MPI_Sendrecv(buf_s_bw[0],n_s_bw*sizeof(rvec),MPI_BYTE,rank_bw,0,
		 buf_r_bw[0],n_r_bw*sizeof(rvec),MPI_BYTE,rank_fw,0,
		 dd->comm->all,&stat[0]);
  }
#endif
}

void dd_move_x(gmx_domdec_t *dd,matrix box,rvec x[],rvec buf[])
{
  int  ncell,nat_tot,n,d,p,i,j,cell;
  int  *index,*cgindex;
  gmx_domdec_comm_t *comm;
  gmx_domdec_comm_dim_t *cd;
  gmx_domdec_ind_t *ind;
  rvec shift,*rbuf;

  comm = dd->comm;
  
  cgindex = dd->cgindex;

  ncell = 1;
  nat_tot = dd->nat_home;
  for(d=0; d<dd->ndim; d++) {
    cd = &comm->cd[d];
    for(p=0; p<cd->np; p++) {
      ind = &cd->ind[p];
      index = ind->index;
      n = 0;
      for(i=0; i<ind->nsend[ncell]; i++) {
	if (dd->ci[dd->dim[d]] == 0) {
	  /* We need to shift the coordinates */
	  copy_rvec(box[dd->dim[d]],shift);
	  for(j=cgindex[index[i]]; j<cgindex[index[i]+1]; j++) {
	    rvec_add(x[j],shift,buf[n]);
	    n++;
	  }
	} else {
	  for(j=cgindex[index[i]]; j<cgindex[index[i]+1]; j++) {
	    copy_rvec(x[j],buf[n]);
	    n++;
	  }
	}
      }

      if (cd->bInPlace)
	rbuf = x + nat_tot;
      else
	rbuf = comm->buf_vr2;
      /* Send and receive the coordinates */
      dd_sendrecv_rvec(dd, d, ddBackward,
		       buf,  ind->nsend[ncell+1],
		       rbuf, ind->nrecv[ncell+1]);
      if (!cd->bInPlace) {
	j = 0;
	for(cell=0; cell<ncell; cell++) {
	  for(i=ind->cell2at0[cell]; i<ind->cell2at1[cell]; i++) {
	    copy_rvec(rbuf[j],x[i]);
	    j++;
	  }
	}
      }
      nat_tot += ind->nrecv[ncell+1];
    }
    ncell += ncell;
  }
}

void dd_move_f(gmx_domdec_t *dd,rvec f[],rvec buf[],rvec *fshift)
{
  int  ncell,nat_tot,n,d,p,i,j,cell;
  int  *index,*cgindex;
  gmx_domdec_comm_t *comm;
  gmx_domdec_comm_dim_t *cd;
  gmx_domdec_ind_t *ind;
  rvec *sbuf;
  ivec vis;
  int  is;

  comm = dd->comm;
  
  cgindex = dd->cgindex;

  ncell = 1;
  nat_tot = dd->nat_home;
  n = 0;
  ncell = dd->ncell/2;
  nat_tot = dd->nat_tot;
  for(d=dd->ndim-1; d>=0; d--) {
    cd = &comm->cd[d];
    for(p=cd->np-1; p>=0; p--) {
      ind = &cd->ind[p];
      nat_tot -= ind->nrecv[ncell+1];
      if (cd->bInPlace) {
	sbuf = f + nat_tot;
      } else {
	sbuf = comm->buf_vr2;
	j = 0;
	for(cell=0; cell<ncell; cell++) {
	  for(i=ind->cell2at0[cell]; i<ind->cell2at1[cell]; i++) {
	    copy_rvec(f[i],sbuf[j]);
	    j++;
	  }
	}
      }
      /* Communicate the forces */
      dd_sendrecv_rvec(dd, d, ddForward,
		       sbuf, ind->nrecv[ncell+1],
		       buf,  ind->nsend[ncell+1]);
      index = ind->index;
      /* Add the received forces */
      n = 0;
      for(i=0; i<ind->nsend[ncell]; i++) {
	if (fshift && dd->ci[dd->dim[d]] == 0) {
	  clear_ivec(vis);
	  vis[dd->dim[d]] = 1;
	  is = IVEC2IS(vis);
	  for(j=cgindex[index[i]]; j<cgindex[index[i]+1]; j++) {
	    rvec_inc(f[j],buf[n]);
	    /* Add this force to the shift force */
	    rvec_inc(fshift[is],buf[n]);
	    n++;
	  }
	} else {
	  for(j=cgindex[index[i]]; j<cgindex[index[i]+1]; j++) {
	    rvec_inc(f[j],buf[n]);
	    n++;
	  }
	}
      }
    }
    ncell /= 2;
  }
}

static void dd_move_cellx(gmx_domdec_t *dd,matrix box)
{
  int  d,d1,dim,pos,i,j,k;
  rvec buf[8],extr_s[2],extr_r[2];
  real len;
  gmx_domdec_comm_t *comm;

  comm = dd->comm;

  comm->cell_d1[0][0] = comm->cell_f0[1];
  comm->cell_d1[0][1] = comm->cell_f1[1];
  if (dd->ndim >= 3) {
    comm->cell_d2[0][0][0] = comm->cell_f0[2];
    comm->cell_d2[0][0][1] = comm->cell_f1[2];
  }

  pos = 0;
  for(d=dd->ndim-2; d>=0; d--) {
    dim  = dd->dim[d];
    /* To use less code we use an rvec to store two reals */
    buf[pos][0] = comm->cell_f0[d+1];
    buf[pos][1] = comm->cell_f1[d+1];
    pos++;
    extr_s[d][0] = comm->cell_f0[d+1];
    extr_s[d][1] = comm->cell_f1[d+1];

    if (d == 0 && dd->ndim >= 3) {
      buf[pos][0] = extr_s[1][0];
      buf[pos][1] = extr_s[1][1];
      pos++;
    }

    if (dd->nc[dim] > 2) {
      /* We only need to communicate the extremes in the forward direction */
      dd_sendrecv_rvec(dd, d, ddForward,
		       extr_s+d, dd->ndim-d-1,
		       extr_r+d, dd->ndim-d-1);
      for(d1=d; d1<dd->ndim-1; d1++) {
	extr_s[d1][0] = max(extr_s[d1][0],extr_r[d1][0]);
	extr_s[d1][1] = min(extr_s[d1][1],extr_r[d1][1]);
      }
    }

    dd_sendrecv_rvec(dd, d, ddBackward, buf, pos, buf+pos, pos);

    if (d == 1 || (d == 0 && dd->ndim == 3)) {
      for(i=d; i<2; i++) {
	comm->cell_d2[1-d][i][0] = buf[pos][0];
	comm->cell_d2[1-d][i][1] = buf[pos][1];
	pos++;
	extr_s[1][0] = max(extr_s[1][0],comm->cell_d2[1-d][i][0]);
	extr_s[1][1] = min(extr_s[1][1],comm->cell_d2[1-d][i][1]);
      }
    }
    if (d == 0) {
      comm->cell_d1[1][0] = buf[pos][0];
      comm->cell_d1[1][1] = buf[pos][1];
      pos++;
      extr_s[0][0] = max(extr_s[0][0],comm->cell_d1[1][0]);
      extr_s[0][1] = min(extr_s[0][1],comm->cell_d1[1][1]);
    }
    if (d == 0 && dd->ndim >= 3) {
      extr_s[1][0] = max(extr_s[1][0],buf[pos][0]);
      extr_s[1][1] = min(extr_s[1][1],buf[pos][1]);
      pos++;
    }
  }

  if (dd->ndim >= 2) {
    dim = dd->dim[1];
    len = box[dim][dim];
    for(i=0; i<2; i++) {
      for(k=0; k<2; k++)
	comm->cell_d1[i][k] *= len;
      dd->cell_ns_x0[dim] = min(dd->cell_ns_x0[dim],comm->cell_d1[i][0]);
      dd->cell_ns_x1[dim] = max(dd->cell_ns_x1[dim],comm->cell_d1[i][1]);
    }
  }
  if (dd->ndim >= 3) {
    dim = dd->dim[2];
    len = box[dim][dim];
    for(i=0; i<2; i++)
      for(j=0; j<2; j++) {
	for(k=0; k<2; k++)
	  comm->cell_d2[i][j][k] *= len;
	dd->cell_ns_x0[dim] = min(dd->cell_ns_x0[dim],comm->cell_d2[i][j][0]);
	dd->cell_ns_x1[dim] = max(dd->cell_ns_x1[dim],comm->cell_d2[i][j][1]);
      }
  }
  for(d=1; d<dd->ndim; d++) {
    comm->cell_f_max0[d] = extr_s[d-1][0];
    comm->cell_f_min1[d] = extr_s[d-1][1];
  }
}

void dd_bcast(gmx_domdec_t *dd,int nbytes,void *data)
{
#ifdef GMX_MPI
  MPI_Bcast(data,nbytes,MPI_BYTE,
	    DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_scatter(gmx_domdec_t *dd,int nbytes,void *src,void *dest)
{
#ifdef GMX_MPI
  MPI_Scatter(src,nbytes,MPI_BYTE,
	      dest,nbytes,MPI_BYTE,
	      DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_gather(gmx_domdec_t *dd,int nbytes,void *src,void *dest)
{
#ifdef GMX_MPI
  MPI_Gather(src,nbytes,MPI_BYTE,
	     dest,nbytes,MPI_BYTE,
	     DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_scatterv(gmx_domdec_t *dd,
			int *scounts,int *disps,void *sbuf,
			int rcount,void *rbuf)
{
#ifdef GMX_MPI
  MPI_Scatterv(sbuf,scounts,disps,MPI_BYTE,
	       rbuf,rcount,MPI_BYTE,
	       DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_gatherv(gmx_domdec_t *dd,
		       int scount,void *sbuf,
		       int *rcounts,int *disps,void *rbuf)
{
#ifdef GMX_MPI
  MPI_Gatherv(sbuf,scount,MPI_BYTE,
	      rbuf,rcounts,disps,MPI_BYTE,
	      DDMASTERRANK(dd),dd->comm->all);
#endif
}

static void dd_collect_cg(gmx_domdec_t *dd)
{
  gmx_domdec_master_t *ma;
  int buf2[2],*ibuf,i;

  ma = dd->ma;

  buf2[0] = dd->ncg_home;
  buf2[1] = dd->nat_home;
  if (DDMASTER(dd)) {
    ibuf = ma->ibuf;
  } else {
    ibuf = NULL;
  }
  /* Collect the charge group and atom counts on the master */
  dd_gather(dd,2*sizeof(int),buf2,ibuf);

  if (DDMASTER(dd)) {
    ma->index[0] = 0;
    for(i=0; i<dd->nnodes; i++) {
      ma->ncg[i] = ma->ibuf[2*i];
      ma->nat[i] = ma->ibuf[2*i+1];
      ma->index[i+1] = ma->index[i] + ma->ncg[i];
      
    }
    /* Make byte counts and indices */
    for(i=0; i<dd->nnodes; i++) {
      ma->ibuf[i] = ma->ncg[i]*sizeof(int);
      ma->ibuf[dd->nnodes+i] = ma->index[i]*sizeof(int);
    }
    if (debug) {
      fprintf(debug,"Initial charge group distribution: ");
      for(i=0; i<dd->nnodes; i++)
	fprintf(debug," %d",ma->ncg[i]);
      fprintf(debug,"\n");
    }
  }
  
  /* Collect the charge group indices on the master */
  dd_gatherv(dd,
	     dd->ncg_home*sizeof(int),dd->index_gl,
	     DDMASTER(dd) ? ma->ibuf : NULL,
	     DDMASTER(dd) ? ma->ibuf+dd->nnodes : NULL,
	     DDMASTER(dd) ? ma->cg : NULL);

  dd->bMasterHasAllCG = TRUE;
}

void dd_collect_vec(gmx_domdec_t *dd,t_block *cgs,rvec *lv,rvec *v)
{
  gmx_domdec_master_t *ma;
  int  n,i,c,a,nalloc=0;
  rvec *buf=NULL;

  ma = dd->ma;

  if (!dd->bMasterHasAllCG)
    dd_collect_cg(dd);

  if (!DDMASTER(dd)) {
#ifdef GMX_MPI
    MPI_Send(lv,dd->nat_home*sizeof(rvec),MPI_BYTE,DDMASTERRANK(dd),
	     dd->rank,dd->comm->all);
#endif
  } else {
    /* Copy the master coordinates to the global array */
    n = DDMASTERRANK(dd);
    a = 0;
    for(i=ma->index[n]; i<ma->index[n+1]; i++)
      for(c=cgs->index[ma->cg[i]]; c<cgs->index[ma->cg[i]+1]; c++)
	copy_rvec(lv[a++],v[c]);
    
    for(n=0; n<dd->nnodes; n++) {
      if (n != dd->rank) {
	if (ma->nat[n] > nalloc) {
	  nalloc = over_alloc_dd(ma->nat[n]);
	  srenew(buf,nalloc);
	}
#ifdef GMX_MPI
	MPI_Recv(buf,ma->nat[n]*sizeof(rvec),MPI_BYTE,DDRANK(dd,n),
		 n,dd->comm->all,MPI_STATUS_IGNORE);
#endif
	a = 0;
	for(i=ma->index[n]; i<ma->index[n+1]; i++)
	  for(c=cgs->index[ma->cg[i]]; c<cgs->index[ma->cg[i]+1]; c++)
	    copy_rvec(buf[a++],v[c]);
      }
    }
    sfree(buf);
  }
}

void dd_collect_state(gmx_domdec_t *dd,t_block *cgs,
		      t_state *state_local,t_state *state)
{
  int i;

  if (DDMASTER(dd)) {
    state->lambda = state_local->lambda;
    copy_mat(state_local->box,state->box);
    copy_mat(state_local->boxv,state->boxv);
    copy_mat(state_local->pcoupl_mu,state->pcoupl_mu);
    for(i=0; i<state_local->ngtc; i++)
      state->nosehoover_xi[i] = state_local->nosehoover_xi[i];
  }
  dd_collect_vec(dd,cgs,state_local->x,state->x);
  if (state_local->flags & STATE_HAS_V)
    dd_collect_vec(dd,cgs,state_local->v,state->v);
  if (state_local->flags & STATE_HAS_SDX)
    dd_collect_vec(dd,cgs,state_local->sd_X,state->sd_X);
  if (state_local->flags & STATE_HAS_CGP)
    dd_collect_vec(dd,cgs,state_local->cg_p,state->cg_p);
}

static void dd_realloc_fr_cg(t_forcerec *fr,int nalloc)
{
  if (debug)
    fprintf(debug,"Reallocating forcerec: currently %d, required %d, allocating %d\n",fr->cg_nalloc,nalloc,over_alloc_dd(nalloc));

  fr->cg_nalloc = over_alloc_dd(nalloc);
  srenew(fr->cg_cm,fr->cg_nalloc);
  srenew(fr->cginfo,fr->cg_nalloc);
}

static void dd_realloc_state(t_state *state,rvec **f,rvec **buf,int nalloc)
{
  if (debug)
    fprintf(debug,"Reallocating state: currently %d, required %d, allocating %d\n",state->nalloc,nalloc,over_alloc_dd(nalloc));

  state->nalloc = over_alloc_dd(nalloc);
  srenew(state->x,state->nalloc);
  if (state->flags & STATE_HAS_V)
    srenew(state->v,state->nalloc);
  if (state->flags & STATE_HAS_SDX)
    srenew(state->sd_X,state->nalloc);
  if (state->flags & STATE_HAS_CGP)
    srenew(state->cg_p,state->nalloc);
  
  srenew(*f,state->nalloc);
  srenew(*buf,state->nalloc);
}

static void dd_distribute_vec(gmx_domdec_t *dd,t_block *cgs,rvec *v,rvec *lv)
{
  gmx_domdec_master_t *ma;
  int  n,i,c,a,nalloc=0;
  rvec *buf=NULL;

  if (DDMASTER(dd)) {
    ma  = dd->ma;

    for(n=0; n<dd->nnodes; n++) {
      if (n != dd->rank) {
	if (ma->nat[n] > nalloc) {
	  nalloc = over_alloc_dd(ma->nat[n]);
	  srenew(buf,nalloc);
	}
	/* Use lv as a temporary buffer */
	a = 0;
	for(i=ma->index[n]; i<ma->index[n+1]; i++)
	  for(c=cgs->index[ma->cg[i]]; c<cgs->index[ma->cg[i]+1]; c++)
	    copy_rvec(v[c],buf[a++]);
	if (a != ma->nat[n])
	  gmx_fatal(FARGS,"Internal error a (%d) != nat (%d)",a,ma->nat[n]);

#ifdef GMX_MPI
	MPI_Send(buf,ma->nat[n]*sizeof(rvec),MPI_BYTE,
		 DDRANK(dd,n),n,dd->comm->all);
#endif
      }
    }
    sfree(buf);
    n = DDMASTERRANK(dd);
    a = 0;
    for(i=ma->index[n]; i<ma->index[n+1]; i++)
      for(c=cgs->index[ma->cg[i]]; c<cgs->index[ma->cg[i]+1]; c++)
	copy_rvec(v[c],lv[a++]);
  } else {
#ifdef GMX_MPI
    MPI_Recv(lv,dd->nat_home*sizeof(rvec),MPI_BYTE,DDMASTERRANK(dd),
	     MPI_ANY_TAG,dd->comm->all,MPI_STATUS_IGNORE);
#endif
  }
}

static void dd_distribute_state(gmx_domdec_t *dd,t_block *cgs,
				t_state *state,t_state *state_local,
				rvec **f,rvec **buf)
{
  int  i;

  if (DDMASTER(dd)) {
    state_local->lambda = state->lambda;
    copy_mat(state->box,state_local->box);
    copy_mat(state->box_rel,state_local->box_rel);
    copy_mat(state->boxv,state_local->boxv);
    for(i=0; i<state_local->ngtc; i++)
      state_local->nosehoover_xi[i] = state->nosehoover_xi[i];
  }
  dd_bcast(dd,sizeof(real),&state_local->lambda);
  dd_bcast(dd,sizeof(state_local->box),state_local->box);
  dd_bcast(dd,sizeof(state_local->box_rel),state_local->box_rel);
  dd_bcast(dd,sizeof(state_local->boxv),state_local->boxv);
  dd_bcast(dd,state_local->ngtc*sizeof(real),state_local->nosehoover_xi);
  if (dd->nat_home > state_local->nalloc)
    dd_realloc_state(state_local,f,buf,dd->nat_home);
  dd_distribute_vec(dd,cgs,state->x,state_local->x);
  if (state_local->flags & STATE_HAS_V)
    dd_distribute_vec(dd,cgs,state->v,state_local->v);
  if (state_local->flags & STATE_HAS_SDX)
    dd_distribute_vec(dd,cgs,state->sd_X,state_local->sd_X);
  if (state_local->flags & STATE_HAS_CGP)
    dd_distribute_vec(dd,cgs,state->cg_p,state_local->cg_p);
}

static char dim2char(int dim)
{
  char c='?';

  switch (dim) {
  case XX: c = 'X'; break;
  case YY: c = 'Y'; break;
  case ZZ: c = 'Z'; break;
  default: gmx_fatal(FARGS,"Unknown dim %d",dim);
  }

  return c;
}

static void write_dd_grid_pdb(char *fn,int step,gmx_domdec_t *dd,matrix box)
{
  rvec grid_s[2],*grid_r=NULL,cx,r;
  char fname[STRLEN],format[STRLEN];
  FILE *out;
  int  a,i,d,z,y,x;
  matrix tric;
  real vol;

  copy_rvec(dd->cell_x0,grid_s[0]);
  copy_rvec(dd->cell_x1,grid_s[1]);

  if (DDMASTER(dd))
    snew(grid_r,2*dd->nnodes);
  
  dd_gather(dd,2*sizeof(rvec),grid_s[0],DDMASTER(dd) ? grid_r[0] : NULL);

  if (DDMASTER(dd)) {
    for(d=0; d<DIM; d++) {
      for(i=0; i<DIM; i++) {
	if (d == i) {
	  tric[d][i] = 1;
	} else {
	  if (dd->nc[d] > 1)
	    tric[d][i] = box[i][d]/box[i][i];
	  else
	    tric[d][i] = 0;
	}
      }
    }
    sprintf(fname,"%s_%d.pdb",fn,step);
    sprintf(format,"%s%s\n",pdbformat,"%6.2f%6.2f");
    out = ffopen(fname,"w");
    gmx_write_pdb_box(out,box);
    a = 1;
    for(i=0; i<dd->nnodes; i++) {
      vol = dd->nnodes/(box[XX][XX]*box[YY][YY]*box[ZZ][ZZ]);
      for(d=0; d<DIM; d++)
	vol *= grid_r[i*2+1][d] - grid_r[i*2][d];
      for(z=0; z<2; z++)
	for(y=0; y<2; y++)
	  for(x=0; x<2; x++) {
	    cx[XX] = grid_r[i*2+x][XX];
	    cx[YY] = grid_r[i*2+y][YY];
	    cx[ZZ] = grid_r[i*2+z][ZZ];
	    mvmul(tric,cx,r);
	    fprintf(out,format,"ATOM",a++,"CA","GLY",' ',1+i,
		    10*r[XX],10*r[YY],10*r[ZZ],1.0,vol);
	  }
      for(d=0; d<DIM; d++) {
	for(x=0; x<4; x++) {
	  switch(d) {
	  case 0: y = 1 + i*8 + 2*x; break;
	  case 1: y = 1 + i*8 + 2*x - (x % 2); break;
	  case 2: y = 1 + i*8 + x; break;
	  }
	  fprintf(out,"%6s%5d%5d\n","CONECT",y,y+(1<<d));
	}
      }
    }
    fclose(out);
    sfree(grid_r);
  }
}

static void write_dd_pdb(char *fn,int step,char *title,t_atoms *atoms,
			 t_commrec *cr,int natoms,
			 rvec x[],matrix box)
{
  char fname[STRLEN],format[STRLEN],format4[STRLEN];
  FILE *out;
  int  i,ii,resnr,c;
  real b;
  gmx_domdec_t *dd;

  dd = cr->dd;

  sprintf(fname,"%s_%d_n%d.pdb",fn,step,cr->sim_nodeid);

  sprintf(format,"%s%s\n",pdbformat,"%6.2f%6.2f");
  sprintf(format4,"%s%s\n",pdbformat4,"%6.2f%6.2f");

  out = ffopen(fname,"w");

  fprintf(out,"TITLE     %s\n",title);
  gmx_write_pdb_box(out,box);
  for(i=0; i<natoms; i++) {
    ii = dd->gatindex[i];
    resnr = atoms->atom[ii].resnr;
    if (i < dd->comm->nat[ddnatZONE]) {
      c = 0;
      while (i >= dd->cgindex[dd->ncg_cell[c+1]]) {
	c++;
      }
      b = c;
    } else if (i < dd->comm->nat[ddnatVSITE]) {
      b = dd->ncell;
    } else {
      b = dd->ncell + 1;
    }
    fprintf(out,strlen(*atoms->atomname[ii])<4 ? format : format4,
	    "ATOM",(ii+1)%100000,
	    *atoms->atomname[ii],*atoms->resname[resnr],' ',(resnr+1)%10000,
	    10*x[i][XX],10*x[i][YY],10*x[i][ZZ],1.0,b);
  }
  fprintf(out,"TER\n");
  
  fclose(out);
}

real dd_cutoff(gmx_domdec_t *dd)
{
  return dd->comm->cutoff;
}

real dd_cutoff_mbody(gmx_domdec_t *dd)
{
  return dd->comm->cutoff_mbody;
}

static void dd_cart_coord2pmecoord(gmx_domdec_t *dd,ivec coord,ivec coord_pme)
{
  int nc,ntot;

  nc   = dd->nc[dd->comm->cartpmedim];
  ntot = dd->comm->ntot[dd->comm->cartpmedim];
  copy_ivec(coord,coord_pme);
  coord_pme[dd->comm->cartpmedim] =
    nc + (coord[dd->comm->cartpmedim]*(ntot - nc) + (ntot - nc)/2)/nc;
}

static int low_ddindex2pmeslab(int ndd,int npme,int ddindex)
{
  /* We add cr->npmenodes/2 to obtain an even distribution */
  return (ddindex*npme + npme/2)/ndd;
}

static int ddindex2pmeslab(const gmx_domdec_t *dd,int ddindex)
{
  return low_ddindex2pmeslab(dd->nnodes,dd->comm->npmenodes,ddindex);
}

static int cr_ddindex2pmeslab(const t_commrec *cr,int ddindex)
{
  return low_ddindex2pmeslab(cr->dd->nnodes,cr->npmenodes,ddindex);
}

static int *dd_pmenodes(t_commrec *cr)
{
  int *pmenodes;
  int n,i,p0,p1;

  snew(pmenodes,cr->npmenodes);
  n = 0;
  for(i=0; i<cr->dd->nnodes; i++) {
    p0 = cr_ddindex2pmeslab(cr,i);
    p1 = cr_ddindex2pmeslab(cr,i+1);
    if (i+1 == cr->dd->nnodes || p1 > p0) {
      if (debug)
	fprintf(debug,"pmenode[%d] = %d\n",n,i+1+n);
      pmenodes[n] = i + 1 + n;
      n++;
    }
  }

  return pmenodes;
}

int gmx_ddcoord2pmeslab(t_commrec *cr,int x,int y,int z)
{
  gmx_domdec_t *dd;
  ivec coords,coords_pme,nc;
  int  slab;

  dd = cr->dd;
  /*
  if (dd->comm->bCartesian) {
    gmx_ddindex2xyz(dd->nc,ddindex,coords);
    dd_coords2pmecoords(dd,coords,coords_pme);
    copy_ivec(dd->ntot,nc);
    nc[dd->cartpmedim]         -= dd->nc[dd->cartpmedim];
    coords_pme[dd->cartpmedim] -= dd->nc[dd->cartpmedim];

    slab = (coords_pme[XX]*nc[YY] + coords_pme[YY])*nc[ZZ] + coords_pme[ZZ];
  } else {
    slab = (ddindex*cr->npmenodes + cr->npmenodes/2)/dd->nnodes;
  }
  */
  coords[XX] = x;
  coords[YY] = y;
  coords[ZZ] = z;
  slab = ddindex2pmeslab(dd,dd_index(dd->nc,coords));

  return slab;
}

static int ddcoord2simnodeid(t_commrec *cr,int x,int y,int z)
{
  gmx_domdec_comm_t *comm;
  ivec coords;
  int  ddindex,nodeid=-1;

  comm = cr->dd->comm;

  coords[XX] = x;
  coords[YY] = y;
  coords[ZZ] = z;
  if (comm->bCartesianPP_PME) {
#ifdef GMX_MPI
    MPI_Cart_rank(cr->mpi_comm_mysim,coords,&nodeid);
#endif
  } else {
    ddindex = dd_index(cr->dd->nc,coords);
    if (comm->bCartesianPP) {
      nodeid = comm->ddindex2simnodeid[ddindex];
    } else {
      if (comm->pmenodes) {
	nodeid = ddindex + gmx_ddcoord2pmeslab(cr,x,y,z);
      } else {
	nodeid = ddindex;
      }
    }
  }
  
  return nodeid;
}

static int dd_simnode2pmenode(t_commrec *cr,int sim_nodeid)
{
  gmx_domdec_t *dd;
  gmx_domdec_comm_t *comm;
  ivec coord,coord_pme;
  int  i;
  int  pmenode=-1;
 
  dd = cr->dd;
  comm = dd->comm;

  /* This assumes a uniform x domain decomposition grid cell size */
  if (comm->bCartesianPP_PME) {
#ifdef GMX_MPI
    MPI_Cart_coords(cr->mpi_comm_mysim,sim_nodeid,DIM,coord);
    if (coord[comm->cartpmedim] < dd->nc[comm->cartpmedim]) {
      /* This is a PP node */
      dd_cart_coord2pmecoord(dd,coord,coord_pme);
      MPI_Cart_rank(cr->mpi_comm_mysim,coord_pme,&pmenode);
    }
#endif
  } else if (comm->bCartesianPP) {
    if (sim_nodeid < dd->nnodes) {
      pmenode = dd->nnodes + ddindex2pmeslab(dd,sim_nodeid);
    }
  } else {
    /* This assumes DD cells with identical x coordinates
     * are numbered sequentially.
     */
    if (dd->comm->pmenodes == NULL) {
      if (sim_nodeid < dd->nnodes) {
	/* The DD index equals the nodeid */
	pmenode = dd->nnodes + ddindex2pmeslab(dd,sim_nodeid);
      }
    } else {
      i = 0;
      while (sim_nodeid > dd->comm->pmenodes[i])
	i++;
      if (sim_nodeid < dd->comm->pmenodes[i])
	pmenode = dd->comm->pmenodes[i];
    }
  }

  return pmenode;
}

bool gmx_pmeonlynode(t_commrec *cr,int sim_nodeid)
{
  bool bPMEOnlyNode;

  if (DOMAINDECOMP(cr)) {
    bPMEOnlyNode = (dd_simnode2pmenode(cr,sim_nodeid) == -1);
  } else {
    bPMEOnlyNode = FALSE;
  }

  return bPMEOnlyNode;
}

void get_pme_ddnodes(t_commrec *cr,int pmenodeid,
		     int *nmy_ddnodes,int **my_ddnodes,int *node_peer)
{
  gmx_domdec_t *dd;
  int x,y,z;
  ivec coord,coord_pme;

  dd = cr->dd;

  snew(*my_ddnodes,(dd->nnodes+cr->npmenodes-1)/cr->npmenodes);
  
  *nmy_ddnodes = 0;
  for(x=0; x<dd->nc[XX]; x++) {
    for(y=0; y<dd->nc[YY]; y++) {
      for(z=0; z<dd->nc[ZZ]; z++) {
	if (dd->comm->bCartesianPP_PME) {
	  coord[XX] = x;
	  coord[YY] = y;
	  coord[ZZ] = z;
	  dd_cart_coord2pmecoord(dd,coord,coord_pme);
	  if (dd->ci[XX] == coord_pme[XX] &&
	      dd->ci[YY] == coord_pme[YY] &&
	      dd->ci[ZZ] == coord_pme[ZZ])
	    (*my_ddnodes)[(*nmy_ddnodes)++] = ddcoord2simnodeid(cr,x,y,z);
	} else {
	  /* The slab corresponds to the nodeid in the PME group */
	  if (gmx_ddcoord2pmeslab(cr,x,y,z) == pmenodeid)
	    (*my_ddnodes)[(*nmy_ddnodes)++] = ddcoord2simnodeid(cr,x,y,z);
	}
      }
    }
  }

  /* The last PP-only node is the peer node */
  *node_peer = (*my_ddnodes)[*nmy_ddnodes-1];

  if (debug) {
    fprintf(debug,"Receive coordinates from PP nodes:");
    for(x=0; x<*nmy_ddnodes; x++)
      fprintf(debug," %d",(*my_ddnodes)[x]);
    fprintf(debug,"\n");
  }
}

static bool receive_vir_ener(t_commrec *cr)
{
  gmx_domdec_comm_t *comm;
  int  pmenode,coords[DIM],rank;
  bool bReceive;

  bReceive = TRUE;
  if (cr->npmenodes < cr->dd->nnodes) {
    comm = cr->dd->comm;
    if (comm->bCartesianPP_PME) {
      pmenode = dd_simnode2pmenode(cr,cr->sim_nodeid);
#ifdef GMX_MPI
      MPI_Cart_coords(cr->mpi_comm_mysim,cr->sim_nodeid,DIM,coords);
      coords[comm->cartpmedim]++;
      if (coords[comm->cartpmedim] < cr->dd->nc[comm->cartpmedim]) {
	MPI_Cart_rank(cr->mpi_comm_mysim,coords,&rank);
	if (dd_simnode2pmenode(cr,rank) == pmenode) {
	  /* This is not the last PP node for pmenode */
	  bReceive = FALSE;
	}
      }
#endif  
    } else {
      pmenode = dd_simnode2pmenode(cr,cr->sim_nodeid);
      if (cr->sim_nodeid+1 < cr->nnodes &&
	  dd_simnode2pmenode(cr,cr->sim_nodeid+1) == pmenode) {
	/* This is not the last PP node for pmenode */
	bReceive = FALSE;
      }
    }
  }
  
  return bReceive;
}

static void rebuild_cgindex(gmx_domdec_t *dd,int *gcgs_index,t_state *state)
{
  int nat,i,*ind,*cgindex,cg_gl;

  ind = state->cg_gl;
  cgindex = dd->cgindex;
  nat = 0;
  cgindex[0] = nat;
  for(i=0; i<state->ncg_gl; i++) {
    cgindex[i] = nat;
    cg_gl = ind[i];
    nat += gcgs_index[cg_gl+1] - gcgs_index[cg_gl];
  }
  cgindex[i] = nat;

  dd->ncg_home = state->ncg_gl;
  dd->nat_home = nat;

  dd->bMasterHasAllCG = FALSE;
}

static void make_dd_indices(gmx_domdec_t *dd,int *gcgs_index,int cg_start,
			    t_forcerec *fr)
{
  int cell,cell1,cg0,cg,cg_gl,a,a_gl;
  int *cell2cg,*cell_ncg1,*index_gl,*gatindex;
  gmx_ga2la_t *ga2la;
  int *cginfo_global,*cginfo;

  if (dd->nat_tot > dd->gatindex_nalloc) {
    dd->gatindex_nalloc = over_alloc_dd(dd->nat_tot);
    srenew(dd->gatindex,dd->gatindex_nalloc);
  }
  
  cell2cg    = dd->ncg_cell;
  cell_ncg1  = dd->comm->cell_ncg1;
  index_gl   = dd->index_gl;
  gatindex   = dd->gatindex;

  cginfo_global = fr->cginfo_global;
  cginfo        = fr->cginfo;

  /* Make the local to global and global to local atom index */
  a = dd->cgindex[cg_start];
  for(cell=0; cell<dd->ncell; cell++) {
    if (cell == 0)
      cg0 = cg_start;
    else
      cg0 = cell2cg[cell];
    for(cg=cg0; cg<cell2cg[cell+1]; cg++) {
      cell1 = cell;
      if (cg - cg0 >= cell_ncg1[cell]) {
	/* Signal that this cg is from more than one cell away */
	cell1 += dd->ncell;
      }
      cg_gl = index_gl[cg];
      for(a_gl=gcgs_index[cg_gl]; a_gl<gcgs_index[cg_gl+1]; a_gl++) {
	gatindex[a] = a_gl;
	ga2la = &dd->ga2la[a_gl];
	ga2la->cell = cell1;
	ga2la->a    = a++;
      }
      cginfo[cg] = cginfo_global[cg_gl];
    }
  }
}

static void clear_dd_indices(gmx_domdec_t *dd,int a_start)
{
  int i;

  /* Clear the indices without looping over all the atoms in the system */
  for(i=a_start; i<dd->nat_tot; i++)
    dd->ga2la[dd->gatindex[i]].cell = -1;

  dd_clear_local_vsite_indices(dd);

  if (dd->constraints)
    dd_clear_local_constraint_indices(dd);
}

static real grid_jump_limit(gmx_domdec_comm_t *comm,int dim_ind)
{
  /* The distance between the boundaries of cells at distance
   * x+-1,y+-1 or y+-1,z+-1 is limited by the cut-off restrictions
   * and by the fact that cells should not be shifted by more than
   * half their size, such that cg's only shift by one cell
   * at redecomposition.
   */
  return max(comm->cutoff_mbody,comm->cutoff/comm->cd[dim_ind].np);
}

static void check_grid_jump(int step,gmx_domdec_t *dd,matrix box)
{
  gmx_domdec_comm_t *comm;
  int  d,dim;
  real limit,bfac;

  comm = dd->comm;
  
  for(d=1; d<dd->ndim; d++) {
    dim = dd->dim[d];
    limit = grid_jump_limit(comm,d);
    bfac = box[dim][dim];
    if (dd->tric_dir[dim])
      bfac *= dd->skew_fac[dim];
    if ((comm->cell_f1[d] - comm->cell_f_max0[d])*bfac <  limit ||
	(comm->cell_f0[d] - comm->cell_f_min1[d])*bfac > -limit)
      gmx_fatal(FARGS,"Step %d: The domain decomposition grid has shifted too much in the %c-direction around cell %d %d %d\n",
		step,dim2char(dim),dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
  }
}

static void set_tric_dir(gmx_domdec_t *dd,matrix box)
{
  int  d,i,j;
  rvec *v;
  real dep,skew_fac2;

  for(d=0; d<DIM; d++) {
    dd->tric_dir[d] = 0;
    for(j=d+1; j<DIM; j++) {
      if (box[j][d] != 0) {
	dd->tric_dir[d] = 1;
	if (dd->nc[j] > 1 && dd->nc[d] == 1)
	  gmx_fatal(FARGS,"Domain decomposition has not been implemented for box vectors that have non-zero components in directions that do not use domain decomposition: ncells = %d %d %d, box vector[%d] = %f %f %f",
		    dd->nc[XX],dd->nc[YY],dd->nc[ZZ],
		    j+1,box[j][XX],box[j][YY],box[j][ZZ]);
      }
    }
    
    /* Convert box vectors to orthogonal vectors for this dimension,
     * for use in distance calculations.
     * Set the trilinic skewing factor that translates
     * the thickness of a slab perpendicular to this dimension
     * into the real thickness of the slab.
     */
    if (dd->tric_dir[d]) {
      skew_fac2 = 1;
      v = dd->comm->v[d];
      if (d == XX || d == YY) {
	/* Normalize such that the "diagonal" is 1 */
	svmul(1/box[d+1][d+1],box[d+1],v[d+1]);
	for(i=0; i<d; i++)
	  v[d+1][i] = 0;
	skew_fac2 -= sqr(v[d+1][d]);
	if (d == XX) {
	  /* Normalize such that the "diagonal" is 1 */
	  svmul(1/box[d+2][d+2],box[d+2],v[d+2]);
	  for(i=0; i<d; i++)
	    v[d+2][i] = 0;
	  /* Make vector [d+2] perpendicular to vector [d+1],
	   * this does not affect the normalization.
	   */
	  dep = iprod(v[d+1],v[d+2])/norm2(v[d+1]);
	  for(i=0; i<DIM; i++)
	    v[d+2][i] -= dep*v[d+1][i];
	  skew_fac2 -= sqr(v[d+2][d]);
	}
	if (debug) {
	  fprintf(debug,"box[%d]  %.3f %.3f %.3f",
		  d,box[d][XX],box[d][YY],box[d][ZZ]);
	  for(i=d+1; i<DIM; i++)
	    fprintf(debug,"  v[%d] %.3f %.3f %.3f",
		    i,v[i][XX],v[i][YY],v[i][ZZ]);
	  fprintf(debug,"\n");
	}
      }
      dd->skew_fac[d] = sqrt(skew_fac2);
    } else {
      dd->skew_fac[d] = 1;
    }
  }
}

static int dd_load_count(gmx_domdec_comm_t *comm)
{
  return (comm->eFlop ? comm->flop_n : comm->cycl_n[ddCyclF]);
}

static float dd_force_load(gmx_domdec_comm_t *comm)
{
  float load;

  if (comm->eFlop) {
    load = comm->flop;
    if (comm->eFlop > 1)
      load *= 1.0 + (comm->eFlop - 1)*(0.1*rand()/RAND_MAX - 0.05);
  } else {
    load = comm->cycl[ddCyclF];
  }

  return load;
}

static void set_pme_x_limits(gmx_domdec_t *dd)
{
  gmx_domdec_comm_t *comm;
  int  nslab,slab,i;
  ivec xyz;

  comm = dd->comm;

  nslab = dd->comm->npmenodes;
  snew(comm->pme_xmin,nslab);
  snew(comm->pme_xmax,nslab);
  for(i=0; i<nslab; i++) {
    comm->pme_xmin[i] = dd->nc[XX] - 1;
    comm->pme_xmax[i] = 0;
  }
  for(i=0; i<dd->nnodes; i++) {
    slab = ddindex2pmeslab(dd,i);
    ddindex2xyz(dd->nc,i,xyz);
    if (xyz[XX] < comm->pme_xmin[slab])
      comm->pme_xmin[slab] = xyz[XX];
    if (xyz[XX] > comm->pme_xmax[slab])
      comm->pme_xmax[slab] = xyz[XX];
  }
}

int dd_pme_maxshift(gmx_domdec_t *dd)
{
  return dd->comm->pme_maxshift;
}

static void set_pme_maxshift(gmx_domdec_t *dd,
			     bool bUniform,matrix box,real *cell_f)
{
  gmx_domdec_comm_t *comm;
  int  pd,ns,nc,s;
  int  *xmin,*xmax;
  real range,limit;
  int  sh;

  comm = dd->comm;
  ns = comm->npmenodes;
  nc = dd->nc[XX];

  if (dd->dim[0] != XX) {
    /* First decomposition is not along x: the worst situation */
    sh = ns/2;
  } else if (ns <= 3 || (bUniform && ns == nc)) {
    /* The optimal situation */
    sh = 1;
  } else {
    /* We need to check for all pme nodes which nodes they
     * could possibly need to communicate with.
     */
    xmin = comm->pme_xmin;
    xmax = comm->pme_xmax;
    range = comm->cellsize_min[XX];
    /* Allow for atoms to be maximally half the cell size or cut-off
     * out of their DD cell.
     */
    range  = 0.5*min(range,comm->cutoff)/dd->skew_fac[XX];
    range /= box[XX][XX];
    /* Avoid unlucky rounding at exactly 0.5 */
    range *= 0.999;

    sh = 1;
    for(s=0; s<ns; s++) {
      limit = cell_f[xmin[s]] - range;
      while (sh+1 < ns &&
	     ((s-(sh+1) >= 0  && cell_f[xmax[s-(sh+1)   ]+1]     > limit) ||
	      (s-(sh+1) <  0  && cell_f[xmax[s-(sh+1)+ns]+1] - 1 > limit))) {
	sh++;
      }
      limit = cell_f[xmax[s]+1] + range;
      while (sh+1 < ns &&
	     ((s+(sh+1) <  ns && cell_f[xmin[s+(sh+1)   ]  ]     < limit) ||
	      (s+(sh+1) >= ns && cell_f[xmin[s+(sh+1)-ns]  ] + 1 < limit))) {
	sh++;
      }
    }
  }
  
  comm->pme_maxshift = sh;

  if (debug)
    fprintf(debug,"PME slab communication range is %d\n",comm->pme_maxshift);
}

static void check_box_size(gmx_domdec_t *dd,matrix box)
{
  int d,dim;
  
  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    if (box[dim][dim]*dd->skew_fac[dim] <
	dd->nc[dim]*dd->comm->cellsize_limit*DD_CELL_MARGIN)
      gmx_fatal(FARGS,"The %c-size of the box (%f) times the triclinic skew factor (%f) is smaller than the number of DD cells (%d) times the smallest allowed cell size (%f)\n",
		dim2char(dim),box[dim][dim],dd->skew_fac[dim],
		dd->nc[dim],dd->comm->cellsize_limit);
  }
}

static void set_dd_cell_sizes_slb(gmx_domdec_t *dd,matrix box,bool bMaster,
				  ivec np)
{
  gmx_domdec_comm_t *comm;
  int  d,j;
  rvec cellsize_min;
  real *cell_x,cell_dx,cellsize;

  comm = dd->comm;

  for(d=0; d<DIM; d++) {
    cellsize_min[d] = box[d][d]*dd->skew_fac[d];
    np[d] = 1;
    if (dd->nc[d] == 1 || comm->slb_frac[d] == NULL) {
      /* Uniform grid */
      cell_dx = box[d][d]/dd->nc[d];
      if (bMaster) {
	for(j=0; j<dd->nc[d]+1; j++)
	  dd->ma->cell_x[d][j] =     j*cell_dx;
      } else {
	dd->cell_x0[d] = (dd->ci[d]  )*cell_dx;
	dd->cell_x1[d] = (dd->ci[d]+1)*cell_dx;
      }
      cellsize = cell_dx*dd->skew_fac[d];
      while (cellsize*np[d] < comm->cutoff)
	np[d]++;
      cellsize_min[d] = cellsize;
    } else {
      /* Statically load balanced grid */
      /* Also when we are not doing a master distribution we determine
       * all cell borders in a loop to obtain identical values to the master
       * distribution case and to determine np.
       */
      if (bMaster) {
	cell_x = dd->ma->cell_x[d];
      } else {
	snew(cell_x,dd->nc[d]+1);
      }
      cell_x[0] = 0;
      for(j=0; j<dd->nc[d]; j++) {
	cell_dx = box[d][d]*comm->slb_frac[d][j];
	cell_x[j+1] = cell_x[j] + cell_dx;
	cellsize = cell_dx*dd->skew_fac[d];
	while (cellsize*np[d] < comm->cutoff)
	  np[d]++;
	cellsize_min[d] = min(cellsize_min[d],cellsize);
      }
      if (!bMaster) {
	dd->cell_x0[d] = cell_x[dd->ci[d]];
	dd->cell_x1[d] = cell_x[dd->ci[d]+1];
	sfree(cell_x);
      }
    }
    /* The following limitation is to avoid that a cell would receive
     * some of its own home charge groups back over the periodic boundary.
     * Double charge groups cause trouble with the global indices.
     */
    if (dd->nc[d] > 1 && np[d] >= dd->nc[d] && DDMASTER(dd)) {
      gmx_fatal(FARGS,"The box size in direction %c (%f) times the triclinic skew factor (%f) is too small for a cut-off of %f with %d domain decomposition cells, use 1 or more than %d cells or increase the box size in this direction",
		dim2char(d),box[d][d],dd->skew_fac[d],comm->cutoff,dd->nc[d],
		dd->nc[d]);
    }
  }

  if (!dd->bDynLoadBal)
    copy_rvec(cellsize_min,comm->cellsize_min);

  if (comm->npmenodes)
    set_pme_maxshift(dd,comm->slb_frac[XX]==NULL,box,comm->pme_dim_f);
}

static void set_dd_cell_sizes_dlb(gmx_domdec_t *dd,matrix box,bool bDynamicBox,
				  bool bUniform,int step)
{
  gmx_domdec_comm_t *comm;
  gmx_domdec_root_t *root=NULL;
  int  d,d1,dim,dim1,i,j,pos,nmin,nmin_old;
  bool bRowMember,bRowRoot,bLimLo,bLimHi;
  real load_aver,load_i,imbalance,change;
  real cellsize_limit_f,dist_min_f,fac,space,halfway;
  real *cell_f_row;
  real change_max = 0.05;
  real relax = 0.5;

  comm = dd->comm;

  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    bRowMember = TRUE;
    bRowRoot = TRUE;
    for(d1=d; d1<dd->ndim; d1++) {
      if (dd->ci[dd->dim[d1]] > 0) {
	if (d1 > d)
	  bRowMember = FALSE;
	bRowRoot = FALSE;
      }
    }
    
    if (bRowRoot) {
      root = comm->root[d];
      /* Store the original boundaries */
      for(i=0; i<dd->nc[dim]+1; i++)
	root->old_cell_f[i] = root->cell_f[i];
      if (bUniform) {
	for(i=0; i<dd->nc[dim]; i++)
	  root->cell_size[i] = 1.0/dd->nc[dim];
      } else if (dd_load_count(comm)) {
	load_aver = comm->load[d].sum_m/dd->nc[dim];
	for(i=0; i<dd->nc[dim]; i++) {
	  /* Determine the relative imbalance of cell i */
	  load_i = comm->load[d].load[i*comm->load[d].nload+2];
	  imbalance = (load_i - load_aver)/load_aver;
	  /* Determine the change of the cell size using underrelaxtion */
	  change = -relax*imbalance;
	  /* Limit the amount of scaling */
	  if (change > change_max)
	    change = change_max;
	  else if (change < -change_max)
	    change = -change_max;
	  /* Set the optimal cell size */
	  root->cell_size[i] *= 1 + change;
	}
      }

      cellsize_limit_f  = comm->cellsize_min[dim]/box[dim][dim];
      cellsize_limit_f *= DD_CELL_MARGIN;
      dist_min_f        = grid_jump_limit(comm,d)/box[dim][dim];
      dist_min_f       *= DD_CELL_MARGIN;
      if (dd->tric_dir[dim]) {
	cellsize_limit_f /= dd->skew_fac[dim];
	dist_min_f       /= dd->skew_fac[dim];
      }
      if (bDynamicBox && d > 0)
	dist_min_f *= DD_PRES_SCALE_MARGIN;

      if (d > 0 && !bUniform) {
	/* Make sure that the grid is not shifted too much */
	for(i=1; i<dd->nc[dim]; i++) {
	  root->bound_min[i] = root->cell_f_max0[i-1] + dist_min_f;
	  space = root->cell_f[i] - (root->cell_f_max0[i-1] + dist_min_f);
	  if (space > 0)
	    root->bound_min[i] += 0.5*space;
	  root->bound_max[i] = root->cell_f_min1[i] - dist_min_f;
	  space = root->cell_f[i] - (root->cell_f_min1[i] - dist_min_f);
	  if (space < 0)
	    root->bound_max[i] += 0.5*space;
	  if (debug)
	    fprintf(debug,
		    "dim %d boundary %d %.3f < %.3f < %.3f < %.3f < %.3f\n",
		    d,i,
		    root->cell_f_max0[i-1] + dist_min_f,
		    root->bound_min[i],root->cell_f[i],root->bound_max[i],
		    root->cell_f_min1[i] - dist_min_f);
	}
      }

      for(i=0; i<dd->nc[dim]; i++)
	root->bCellMin[i] = FALSE;
      nmin = 0;
      do {
	nmin_old = nmin;
	/* We need the total for normalization */
	fac = 0;
	for(i=0; i<dd->nc[dim]; i++) {
	  if (root->bCellMin[i] == FALSE)
	    fac += root->cell_size[i];
	}
	fac = (1 - nmin*dist_min_f)/fac;
	/* Determine the cell boundaries */
	root->cell_f[0] = 0;
	for(i=0; i<dd->nc[dim]; i++) {
	  if (root->bCellMin[i] == FALSE) {
	    root->cell_size[i] *= fac;
	    if (root->cell_size[i] < cellsize_limit_f) {
	      root->bCellMin[i] = TRUE;
	      root->cell_size[i] = cellsize_limit_f;
	      nmin++;
	    }
	  }
	  root->cell_f[i+1] = root->cell_f[i] + root->cell_size[i];
	}
      } while (nmin > nmin_old);

      /* Set the last boundary to exactly 1 */
      i = dd->nc[dim] - 1;
      root->cell_f[i+1] = 1;
      root->cell_size[i] = root->cell_f[i+1] - root->cell_f[i];
      /* For this check we should not use DD_CELL_MARGIN,
       * but a slightly smaller factor,
       * since rounding could get use below the limit.
       */
      if (root->cell_size[i] < cellsize_limit_f*DD_CELL_MARGIN2/DD_CELL_MARGIN)
	gmx_fatal(FARGS,"Step %d: the dynamic load balancing could not balance dimension %c: box size %f, triclinic skew factor %f, #cells %d, minimum cell size %f\n",
		  step,dim2char(dim),box[dim][dim],dd->skew_fac[dim],
		  dd->nc[dim],comm->cellsize_min[dim]);

      root->bLimited = (nmin > 0);
      
      if (!bUniform) {
	/* Check if the boundary did not displace more than halfway
	 * each of the cells it bounds, as this could cause problems,
	 * especially when the differences between cell sizes are large.
	 * If changes are applied, they will not make cells smaller
	 * than the cut-off, as we check all the boundaries which
	 * might be affected by a change and if the old state was ok,
	 * the cells will at most be shrunk back to their old size.
	 */
	for(i=1; i<dd->nc[dim]; i++) {
	  halfway = 0.5*(root->old_cell_f[i] + root->old_cell_f[i-1]);
	  if (root->cell_f[i] < halfway) {
	    root->cell_f[i] = halfway;
	    /* Check if the change also causes shifts of the next boundaries */
	    for(j=i+1; j<dd->nc[dim]; j++) {
	      if (root->cell_f[j] < root->cell_f[j-1] + cellsize_limit_f)
		root->cell_f[j] =  root->cell_f[j-1] + cellsize_limit_f;
	    }
	  }
	  halfway = 0.5*(root->old_cell_f[i] + root->old_cell_f[i+1]);
	  if (root->cell_f[i] > halfway) {
	    root->cell_f[i] = halfway;
	    /* Check if the change also causes shifts of the next boundaries */
	    for(j=i-1; j>=1; j--) {
	      if (root->cell_f[j] > root->cell_f[j+1] - cellsize_limit_f)
		root->cell_f[j] = root->cell_f[j+1] - cellsize_limit_f;
	    }
	  }
	}
      }

      if (d > 0) {
	/* Take care of the staggering of the cell boundaries */
	if (bUniform) {
	  for(i=0; i<dd->nc[dim]; i++) {
	    root->cell_f_max0[i] = root->cell_f[i];
	    root->cell_f_min1[i] = root->cell_f[i+1];
	  }
	} else {
	  for(i=1; i<dd->nc[dim]; i++) {
	    bLimLo = (root->cell_f[i] < root->bound_min[i]);
	    bLimHi = (root->cell_f[i] > root->bound_max[i]);
	    if (bLimLo && bLimHi) {
	      /* Both limits violated, try the best we can */
	      root->cell_f[i] = 0.5*(root->bound_min[i] + root->bound_max[i]);
	    } else if (bLimLo) {
	      root->cell_f[i] = root->bound_min[i];
	    } else if (bLimHi) {
	      root->cell_f[i] = root->bound_max[i];
	    }
	    if (bLimLo || bLimHi)
	      root->bLimited = TRUE;
	  }
	}
      }
      /* After the checks above, the cells should obey the cut-off
       * restrictions, but it does not hurt to check.
       */
      for(i=0; i<dd->nc[dim]; i++)
	if (root->cell_f[i+1] - root->cell_f[i] <
	    cellsize_limit_f/DD_CELL_MARGIN)
	  fprintf(stderr,
		  "\nWARNING step %d: direction %c, cell %d too small: %f\n",
		  step,dim2char(dim),i,
		  (root->cell_f[i+1] - root->cell_f[i])
		  *box[dim][dim]*dd->skew_fac[dim]);
    
      pos = dd->nc[dim] + 1;
      /* Store the cell boundaries of the lower dimensions at the end */
      for(d1=0; d1<d; d1++) {
	root->cell_f[pos++] = comm->cell_f0[d1];
	root->cell_f[pos++] = comm->cell_f1[d1];
      }
      
      if (d == 0 && comm->npmenodes > 0) {
	/* The master determines the maximum shift for
	 * the coordinate communication between separate PME nodes.
	 */
	set_pme_maxshift(dd,bUniform,box,root->cell_f);
      }
      root->cell_f[pos++] = comm->pme_maxshift;
    }

    if (bRowMember) {
      if (bRowRoot)
	cell_f_row = root->cell_f;
      else
	cell_f_row = comm->cell_f_row;
#ifdef GMX_MPI
      /* Each node would only need to know two fractions,
       * but it is probably cheaper to broadcast the whole array.
       */
      MPI_Bcast(cell_f_row,DD_CELL_F_SIZE(dd,d)*sizeof(real),MPI_BYTE,
		0,dd->comm->mpi_comm_load[d]);
#endif
      /* Copy the fractions for this dimension from the buffer */
      comm->cell_f0[d] = cell_f_row[dd->ci[dim]  ];
      comm->cell_f1[d] = cell_f_row[dd->ci[dim]+1];
      pos = dd->nc[dim] + 1;
      for(d1=0; d1<=d; d1++) {
	if (d1 < d) {
	  /* Copy the cell fractions of the lower dimensions */
	  comm->cell_f0[d1] = cell_f_row[pos++];
	  comm->cell_f1[d1] = cell_f_row[pos++];
	}
	/* Set the cell dimensions */
	dim1 = dd->dim[d1];
	dd->cell_x0[dim1] = comm->cell_f0[d1]*box[dim1][dim1];
	dd->cell_x1[dim1] = comm->cell_f1[d1]*box[dim1][dim1];
      }
      comm->pme_maxshift = (int)(cell_f_row[pos++] + 0.5);
    }
  }
  
  /* Set the dimensions for which no DD is used */
  for(dim=0; dim<DIM; dim++) {
    if (dd->nc[dim] == 1) {
      dd->cell_x0[dim] = 0;
      dd->cell_x1[dim] = box[dim][dim];
    }
  }
}

static void realloc_comm_ind(gmx_domdec_t *dd,ivec npulse)
{
  int d,np,i;
  gmx_domdec_comm_dim_t *cd;

  for(d=0; d<dd->ndim; d++) {
    cd = &dd->comm->cd[d];
    np = npulse[dd->dim[d]];
    if (np > cd->np_nalloc) {
      if (debug)
	fprintf(debug,"(Re)allocing cd for %c to %d pulses\n",
		dim2char(dd->dim[d]),np);
      if (DDMASTER(dd) && cd->np_nalloc > 0)
	fprintf(stderr,"\nIncreasing the number of cell to communicate in dimension %c to %d for the first time\n",dim2char(dd->dim[d]),np);
      srenew(cd->ind,np);
      for(i=cd->np_nalloc; i<np; i++) {
	cd->ind[i].index  = NULL;
	cd->ind[i].nalloc = 0;
      }
      cd->np_nalloc = np;
    }
    cd->np = np;
  }
}


static void set_dd_cell_sizes(gmx_domdec_t *dd,matrix box,bool bDynamicBox,
			      bool bUniform,bool bMaster,int step)
{
  int  d;
  ivec np;

  /* Copy the old cell boundaries for the cg displacement check */
  copy_rvec(dd->cell_x0,dd->comm->old_cell_x0);
  copy_rvec(dd->cell_x1,dd->comm->old_cell_x1);

  set_tric_dir(dd,box);

  if (dd->bDynLoadBal) {
    if (DDMASTER(dd))
      check_box_size(dd,box);

    if (bMaster) {
      set_dd_cell_sizes_slb(dd,box,bMaster,np);
    } else {
      set_dd_cell_sizes_dlb(dd,box,bDynamicBox,bUniform,step);
    }
  } else {
    set_dd_cell_sizes_slb(dd,box,bMaster,np);
    realloc_comm_ind(dd,np);
  }

  if (debug)
    for(d=0; d<DIM; d++)
      fprintf(debug,"cell_x[%d] %f - %f skew_fac %f\n",
	      d,dd->cell_x0[d],dd->cell_x1[d],dd->skew_fac[d]);
}

static void set_dd_ns_cell_sizes(gmx_domdec_t *dd,matrix box,int step)
{
  int dim_ind,dim;

  for(dim_ind=0; dim_ind<dd->ndim; dim_ind++) {
    dim = dd->dim[dim_ind];
    
    if (dd->bDynLoadBal &&
	(dd->cell_x1[dim] - dd->cell_x0[dim])*dd->skew_fac[dim] <
	dd->comm->cellsize_min[dim])
      gmx_fatal(FARGS,"Step %d: The %c-size (%f) times the triclinic skew factor (%f) is smaller than the smallest allowed cell size (%f) for domain decomposition grid cell %d %d %d",
		step,dim2char(dim),
		dd->cell_x1[dim] - dd->cell_x0[dim],dd->skew_fac[dim],
		dd->comm->cellsize_min[dim],
		dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
  }

  /* Set the size of the ns grid,
   * for dynamic load balancing this is corrected in dd_move_cellx.
   */
  copy_rvec(dd->cell_x0,dd->cell_ns_x0);
  copy_rvec(dd->cell_x1,dd->cell_ns_x1);
  
  if (dd->bGridJump && dd->ndim > 1) {
    dd_move_cellx(dd,box);
    check_grid_jump(step,dd,box);
  }
}

static void make_tric_corr_matrix(matrix box,matrix tcm)
{
  tcm[YY][XX] = -box[YY][XX]/box[YY][YY];
  if (box[ZZ][ZZ] > 0) {
    tcm[ZZ][XX] = -(box[ZZ][YY]*tcm[YY][XX] + box[ZZ][XX])/box[ZZ][ZZ];
    tcm[ZZ][YY] = -box[ZZ][YY]/box[ZZ][ZZ];
  } else {
    tcm[ZZ][XX] = 0;
    tcm[ZZ][YY] = 0;
  }
}

static void distribute_cg(FILE *fplog,int step,
			  matrix box,t_block *cgs,rvec pos[],
			  gmx_domdec_t *dd)
{
  gmx_domdec_master_t *ma;
  int **tmp_ind=NULL,*tmp_nalloc=NULL;
  int  i,icg,j,k,k0,k1,d;
  matrix tcm;
  rvec cg_cm;
  ivec ind;
  real nrcg,inv_ncg,pos_d;
  atom_id *cgindex;

  ma = dd->ma;

  /* Set the cell boundaries */
  set_dd_cell_sizes(dd,box,FALSE,TRUE,TRUE,step);

  if (tmp_ind == NULL) {
    snew(tmp_nalloc,dd->nnodes);
    snew(tmp_ind,dd->nnodes);
    for(i=0; i<dd->nnodes; i++) {
      tmp_nalloc[i] = over_alloc_large(cgs->nr/dd->nnodes+1);
      snew(tmp_ind[i],tmp_nalloc[i]);
    }
  }

  /* Clear the count */
  for(i=0; i<dd->nnodes; i++) {
    ma->ncg[i] = 0;
    ma->nat[i] = 0;
  }

  make_tric_corr_matrix(box,tcm);

  cgindex = cgs->index;
  
  /* Compute the center of geometry for all charge groups */
  for(icg=0; icg<cgs->nr; icg++) {
    k0      = cgindex[icg];
    k1      = cgindex[icg+1];
    nrcg    = k1 - k0;
    if (nrcg == 1) {
      copy_rvec(pos[k0],cg_cm);
    }
    else {
      inv_ncg = 1.0/nrcg;
      
      clear_rvec(cg_cm);
      for(k=k0; (k<k1); k++)
	rvec_inc(cg_cm,pos[k]);
      for(d=0; (d<DIM); d++)
	cg_cm[d] = inv_ncg*cg_cm[d];
    }
    /* Put the charge group in the box and determine the cell index */
    for(d=DIM-1; d>=0; d--) {
      pos_d = cg_cm[d];
      if (dd->tric_dir[d] && dd->nc[d] > 1) {
	/* Use triclinic coordintates for this dimension */
	for(j=d+1; j<DIM; j++)
	  pos_d += cg_cm[j]*tcm[j][d];
      }
      while(pos_d >= box[d][d]) {
	pos_d -= box[d][d];
	rvec_dec(cg_cm,box[d]);
	for(k=k0; (k<k1); k++)
	  rvec_dec(pos[k],box[d]);
      }
      while(pos_d < 0) {
	pos_d += box[d][d];
	rvec_inc(cg_cm,box[d]);
	for(k=k0; (k<k1); k++)
	  rvec_inc(pos[k],box[d]);
      }
      /* This could be done more efficiently */
      ind[d] = 0;
      while(ind[d]+1 < dd->nc[d] && pos_d >= ma->cell_x[d][ind[d]+1])
	ind[d]++;
    }
    i = dd_index(dd->nc,ind);
    if (ma->ncg[i] == tmp_nalloc[i]) {
      tmp_nalloc[i] = over_alloc_large(ma->ncg[i]+1);
      srenew(tmp_ind[i],tmp_nalloc[i]);
    }
    tmp_ind[i][ma->ncg[i]] = icg;
    ma->ncg[i]++;
    ma->nat[i] += cgindex[icg+1] - cgindex[icg];
  }
  
  k1 = 0;
  for(i=0; i<dd->nnodes; i++) {
    ma->index[i] = k1;
    for(k=0; k<ma->ncg[i]; k++)
      ma->cg[k1++] = tmp_ind[i][k];
  }
  ma->index[dd->nnodes] = k1;

  for(i=0; i<dd->nnodes; i++)
    sfree(tmp_ind[i]);
  sfree(tmp_ind);
  sfree(tmp_nalloc);

  if (fplog) {
    fprintf(fplog,"Charge group distribution at step %d:",step);
    for(i=0; i<dd->nnodes; i++)
      fprintf(fplog," %d",ma->ncg[i]);
    fprintf(fplog,"\n");
  }
}

static void get_cg_distribution(FILE *fplog,int step,gmx_domdec_t *dd,
				t_block *cgs,matrix box,rvec pos[])
{
  gmx_domdec_master_t *ma=NULL;
  int i,cg_gl;
  int *ibuf,buf2[2] = { 0, 0 };

  clear_dd_indices(dd,0);

  if (DDMASTER(dd)) {
    ma = dd->ma;
    if (ma->ncg == NULL) {
      snew(ma->ncg,dd->nnodes);
      snew(ma->index,dd->nnodes+1);
      snew(ma->cg,cgs->nr);
      snew(ma->nat,dd->nnodes);
      snew(ma->ibuf,dd->nnodes*2);
      snew(ma->cell_x,DIM);
      for(i=0; i<DIM; i++)
	snew(ma->cell_x[i],dd->nc[i]+1);
    }      
    
    distribute_cg(fplog,step,box,cgs,pos,dd);
    for(i=0; i<dd->nnodes; i++) {
      ma->ibuf[2*i]   = ma->ncg[i];
      ma->ibuf[2*i+1] = ma->nat[i];
    }
    ibuf = ma->ibuf;
  } else {
    ibuf = NULL;
  }
  dd_scatter(dd,2*sizeof(int),ibuf,buf2);

  dd->ncg_home = buf2[0];
  dd->nat_home = buf2[1];
  if (dd->ncg_home > dd->cg_nalloc || dd->cg_nalloc == 0) {
    dd->cg_nalloc = over_alloc_dd(dd->ncg_home);
    srenew(dd->index_gl,dd->cg_nalloc);
    srenew(dd->cgindex,dd->cg_nalloc+1);
  }
  if (DDMASTER(dd)) {
    for(i=0; i<dd->nnodes; i++) {
      ma->ibuf[i] = ma->ncg[i]*sizeof(int);
      ma->ibuf[dd->nnodes+i] = ma->index[i]*sizeof(int);
    }
  }

  dd_scatterv(dd,
	      DDMASTER(dd) ? ma->ibuf : NULL,
	      DDMASTER(dd) ? ma->ibuf+dd->nnodes : NULL,
	      DDMASTER(dd) ? ma->cg : NULL,
	      dd->ncg_home*sizeof(int),dd->index_gl);

  /* Determine the home charge group sizes */
  dd->cgindex[0] = 0;
  for(i=0; i<dd->ncg_home; i++) {
    cg_gl = dd->index_gl[i];
    dd->cgindex[i+1] =
      dd->cgindex[i] + cgs->index[cg_gl+1] - cgs->index[cg_gl];
  }

  if (debug) {
    fprintf(debug,"Home charge groups:\n");
    for(i=0; i<dd->ncg_home; i++) {
      fprintf(debug," %d",dd->index_gl[i]);
      if (i % 10 == 9) 
	fprintf(debug,"\n");
    }
    fprintf(debug,"\n");
  }

  dd->bMasterHasAllCG = TRUE;
}

static int compact_and_copy_vec_at(int ncg,int *move,
				   int *cgindex,
				   int nvec,int vec,
				   rvec *src,gmx_domdec_comm_t *comm,
				   bool bCompact)
{
  int m,icg,i,i0,i1,nrcg;
  int home_pos;
  int pos_vec[DIM*2];

  home_pos = 0;

  for(m=0; m<DIM*2; m++)
    pos_vec[m] = 0;

  i0 = 0;
  for(icg=0; icg<ncg; icg++) {
    i1 = cgindex[icg+1];
    m = move[icg];
    if (m == -1) {
      if (bCompact) {
	/* Compact the home array in place */
	for(i=i0; i<i1; i++)
	  copy_rvec(src[i],src[home_pos++]);
      }
    } else {
      /* Copy to the communication buffer */
      nrcg = i1 - i0;
      pos_vec[m] += 1 + vec*nrcg;
      for(i=i0; i<i1; i++)
	copy_rvec(src[i],comm->cgcm_state[m][pos_vec[m]++]);
      pos_vec[m] += (nvec - vec - 1)*nrcg;
    }
    if (!bCompact)
      home_pos += i1 - i0;
    i0 = i1;
  }

  return home_pos;
}

static int compact_and_copy_vec_cg(int ncg,int *move,
				   int *cgindex,
				   int nvec,rvec *src,gmx_domdec_comm_t *comm,
				   bool bCompact)
{
  int m,icg,i0,i1,nrcg;
  int home_pos;
  int pos_vec[DIM*2];

  home_pos = 0;

  for(m=0; m<DIM*2; m++)
    pos_vec[m] = 0;

  i0 = 0;
  for(icg=0; icg<ncg; icg++) {
    i1 = cgindex[icg+1];
    m = move[icg];
    if (m == -1) {
      if (bCompact) {
	/* Compact the home array in place */
	copy_rvec(src[icg],src[home_pos++]);
      }
    } else {
      nrcg = i1 - i0;
      /* Copy to the communication buffer */
      copy_rvec(src[icg],comm->cgcm_state[m][pos_vec[m]]);
      pos_vec[m] += 1 + nrcg*nvec;
    }
    i0 = i1;
  }
  if (!bCompact)
    home_pos = ncg;

  return home_pos;
}

static int compact_ind(int ncg,int *move,
		       int *index_gl,int *cgindex,
		       int *gatindex,gmx_ga2la_t *ga2la,
		       int *cginfo)
{
  int cg,nat,a0,a1,a,a_gl;
  int home_pos;
  
  home_pos = 0;
  nat = 0;
  for(cg=0; cg<ncg; cg++) {
    a0 = cgindex[cg];
    a1 = cgindex[cg+1];
    if (move[cg] == -1) {
      /* Compact the home arrays in place.
       * Anything that can be done here avoids access to global arrays.
       */
      cgindex[home_pos] = nat;
      for(a=a0; a<a1; a++) {
	a_gl = gatindex[a];
	gatindex[nat] = a_gl;
	/* The cell number stays 0, so we don't need to set it */
	ga2la[a_gl].a = nat;
	nat++;
      }
      index_gl[home_pos] = index_gl[cg];
      cginfo[home_pos]   = cginfo[cg];
      home_pos++;
    } else {
      /* Clear the global indices */
      for(a=a0; a<a1; a++) {
	a_gl = gatindex[a];
	ga2la[a_gl].cell = -1;
      }
    }
  }
  cgindex[home_pos] = nat;
  
  return home_pos;
}

static void clear_and_mark_ind(int ncg,int *move,
			       int *cgindex,int *gatindex,
			       gmx_ga2la_t *ga2la,
			       int *cell_index)
{
  int cg,a0,a1,a,a_gl;
  
  for(cg=0; cg<ncg; cg++) {
    if (move[cg] >= 0) {
      a0 = cgindex[cg];
      a1 = cgindex[cg+1];
      /* Clear the global indices */
      for(a=a0; a<a1; a++) {
	a_gl = gatindex[a];
	ga2la[a_gl].cell = -1;
      }
      /* Signal that this cg has moved using the ns cell index.
       * Here we set it to -1.
       * fill_grid will change it from -1 to 4*grid->ncells.
       */
      cell_index[cg] = -1;
    }
  }
}

static void print_cg_move(FILE *fplog,
			  gmx_domdec_t *dd,int step,int cg,int dim,int dir,
			  real limitd,
			  rvec cm_old,rvec cm_new,real pos_d)
{
  fprintf(fplog,"\nStep %d:\n",step);
  fprintf(fplog,"The charge group starting at atom %d moved than the distance allowed by the domain decomposition (%f) in direction %c\n",
	  glatnr(dd,dd->cgindex[cg]),limitd,dim2char(dim));
  fprintf(fplog,"distance out of cell %f\n",
	  dir==1 ? pos_d - dd->cell_x1[dim] : pos_d - dd->cell_x0[dim]);
  fprintf(fplog,"Old coordinates: %8.3f %8.3f %8.3f\n",
	  cm_old[XX],cm_old[YY],cm_old[ZZ]);
  fprintf(fplog,"New coordinates: %8.3f %8.3f %8.3f\n",
	  cm_new[XX],cm_new[YY],cm_new[ZZ]);
  fprintf(fplog,"Old cell boundaries in direction %c: %8.3f %8.3f\n",
	  dim2char(dim),
	  dd->comm->old_cell_x0[dim],dd->comm->old_cell_x1[dim]);
  fprintf(fplog,"New cell boundaries in direction %c: %8.3f %8.3f\n",
	  dim2char(dim),
	  dd->cell_x0[dim],dd->cell_x1[dim]);
}

static void cg_move_error(FILE *fplog,
			  gmx_domdec_t *dd,int step,int cg,int dim,int dir,
			  real limitd,
			  rvec cm_old,rvec cm_new,real pos_d)
{
  if (fplog)
    print_cg_move(fplog, dd,step,cg,dim,dir,limitd,cm_old,cm_new,pos_d);
  print_cg_move(stderr,dd,step,cg,dim,dir,limitd,cm_old,cm_new,pos_d);
  gmx_fatal(FARGS,"A charge group move too far between two domain decomposition steps");
}

static int dd_redistribute_cg(FILE *fplog,int step,
			      gmx_domdec_t *dd,t_block *gcgs,
			      t_state *state,rvec **f,rvec **buf,
			      t_forcerec *fr,t_mdatoms *md,
			      bool bCompact,
			      t_nrnb *nrnb)
{
  int  *move;
  int  ncg[DIM*2],nat[DIM*2];
  int  c,i,cg,k,k0,k1,d,dim,dim2,dir,d2,d3,d4,cell_d;
  int  mc,cdd,nrcg,ncg_recv,nat_recv,nvs,nvr,nvec,vec;
  int  sbuf[2],rbuf[2];
  int  home_pos_cg,home_pos_at,ncg_stay_home,buf_pos;
  int  flag;
  bool bV,bSDX,bCGP;
  ivec tric_dir,dev;
  real inv_ncg,pos_d;
  matrix tcm;
  rvec *cg_cm,cell_x0,cell_x1,limitd,limit0,limit1,cm_new;
  atom_id *cgindex;
  gmx_domdec_comm_t *comm;

  comm  = dd->comm;
  cg_cm = fr->cg_cm;

  bV   = (state->flags & STATE_HAS_V);
  bSDX = (state->flags & STATE_HAS_SDX);
  bCGP = (state->flags & STATE_HAS_CGP);

  if (dd->ncg_tot > comm->nalloc_int) {
    comm->nalloc_int = over_alloc_dd(dd->ncg_tot);
    srenew(comm->buf_int,comm->nalloc_int);
  }
  move = comm->buf_int;

  /* Clear the count */
  for(c=0; c<dd->ndim*2; c++) {
    ncg[c] = 0;
    nat[c] = 0;
  }

  for(d=0; (d<DIM); d++) {
    limitd[d] = dd->comm->cellsize_min[d];
    cell_x0[d] = dd->cell_x0[d];
    cell_x1[d] = dd->cell_x1[d];
    c = dd->ci[d] - 1;
    if (c < 0)
      c = dd->nc[d] - 1;
    limit0[d] = comm->old_cell_x0[d] - limitd[d];
    c = dd->ci[d] + 1;
    if (c >= dd->nc[d])
      c = 0;
    limit1[d] = comm->old_cell_x1[d] + limitd[d];
    if (dd->tric_dir[d] && dd->nc[d] > 1)
      tric_dir[d] = 1;
    else
      tric_dir[d] = 0;
  }

  make_tric_corr_matrix(state->box,tcm);

  cgindex = dd->cgindex;

  /* Compute the center of geometry for all home charge groups
   * and put them in the box and determine where they should go.
   */
  for(cg=0; cg<dd->ncg_home; cg++) {
    k0   = cgindex[cg];
    k1   = cgindex[cg+1];
    nrcg = k1 - k0;
    if (nrcg == 1) {
      copy_rvec(state->x[k0],cm_new);
    }
    else {
      inv_ncg = 1.0/nrcg;
      
      clear_rvec(cm_new);
      for(k=k0; (k<k1); k++)
	rvec_inc(cm_new,state->x[k]);
      for(d=0; (d<DIM); d++)
	cm_new[d] = inv_ncg*cm_new[d];
    }

    for(d=DIM-1; d>=0; d--) {
      if (dd->nc[d] > 1) {
	/* Determine the location of this cg in lattice coordinates */
	pos_d = cm_new[d];
	if (tric_dir[d]) {
	  for(d2=d+1; d2<DIM; d2++)
	    pos_d += cm_new[d2]*tcm[d2][d];
	}
	/* Put the charge group in the triclinic unit-cell */
	if (pos_d >= cell_x1[d]) {
	  if (pos_d >= limit1[d])
	    cg_move_error(fplog,dd,step,cg,d,1,limitd[d],
			  cg_cm[cg],cm_new,pos_d);
	  dev[d] = 1;
	  if (dd->ci[d] == dd->nc[d] - 1) {
	    rvec_dec(cm_new,state->box[d]);
	    for(k=k0; (k<k1); k++)
	      rvec_dec(state->x[k],state->box[d]);
	  }
	} else if (pos_d < cell_x0[d]) {
	  if (pos_d < limit0[d])
	    cg_move_error(fplog,dd,step,cg,d,-1,limitd[d],
			  cg_cm[cg],cm_new,pos_d);
	  dev[d] = -1;
	  if (dd->ci[d] == 0) {
	    rvec_inc(cm_new,state->box[d]);
	    for(k=k0; (k<k1); k++)
	      rvec_inc(state->x[k],state->box[d]);
	  }
	} else {
	  dev[d] = 0;
	}
      } else {
	/* Put the charge group in the rectangular unit-cell */
	while (cm_new[d] >= state->box[d][d]) {
	  rvec_dec(cm_new,state->box[d]);
	  for(k=k0; (k<k1); k++)
	    rvec_dec(state->x[k],state->box[d]);
	}
	while (cm_new[d] < 0) {
	  rvec_inc(cm_new,state->box[d]);
	  for(k=k0; (k<k1); k++)
	    rvec_inc(state->x[k],state->box[d]);
	}
      }
    }
    
    copy_rvec(cm_new,cg_cm[cg]);

    /* Determine where this cg should go */
    flag = 0;
    mc = -1;
    for(d=0; d<dd->ndim; d++) {
      dim = dd->dim[d];
      if (dev[dim] == 1) {
	flag |= DD_FLAG_FW(d);
	if (mc == -1)
	  mc = d*2;
      } else if (dev[dim] == -1) {
	flag |= DD_FLAG_BW(d);
	if (mc == -1) {
	  if (dd->nc[dim] > 2)
	    mc = d*2 + 1;
	  else
	    mc = d*2;
	}
      }
    }
    move[cg] = mc;
    if (mc >= 0) {
      if (ncg[mc]+1 > comm->cggl_flag_nalloc[mc]) {
	comm->cggl_flag_nalloc[mc] = over_alloc_dd(ncg[mc]+1);
	srenew(comm->cggl_flag[mc],comm->cggl_flag_nalloc[mc]*DD_CGIBS);
      }
      comm->cggl_flag[mc][ncg[mc]*DD_CGIBS  ] = dd->index_gl[cg];
      /* We store the cg size in the lower 16 bits
       * and the place where the charge group should go
       * in the next 6 bits. This saves some communication volume.
       */
      comm->cggl_flag[mc][ncg[mc]*DD_CGIBS+1] = nrcg | flag;
      ncg[mc] += 1;
      nat[mc] += nrcg;
    }
  }

  inc_nrnb(nrnb,eNR_CGCM,dd->nat_home);
  inc_nrnb(nrnb,eNR_RESETX,dd->ncg_home);

  nvec = 1;
  if (bV)
    nvec++;
  if (bSDX)
    nvec++;
  if (bCGP)
    nvec++;

  /* Make sure the communication buffers are large enough */
  for(mc=0; mc<dd->ndim*2; mc++) {
    nvr = ncg[mc] + nat[mc]*nvec;
    if (nvr>comm->cgcm_state_nalloc[mc]) {
      comm->cgcm_state_nalloc[mc] = over_alloc_dd(nvr);
      srenew(comm->cgcm_state[mc],comm->cgcm_state_nalloc[mc]);
    }
  }

  /* Recalculating cg_cm might be cheaper than communicating,
   * but that could give rise to rounding issues.
   */
  home_pos_cg =
    compact_and_copy_vec_cg(dd->ncg_home,move,cgindex,
			    nvec,cg_cm,comm,bCompact);

  vec = 0;
  home_pos_at =
    compact_and_copy_vec_at(dd->ncg_home,move,cgindex,
			    nvec,vec++,state->x,comm,bCompact);
  if (bV)
    compact_and_copy_vec_at(dd->ncg_home,move,cgindex,
			    nvec,vec++,state->v,comm,bCompact);
  if (bSDX)
    compact_and_copy_vec_at(dd->ncg_home,move,cgindex,
			    nvec,vec++,state->sd_X,comm,bCompact);
  if (bCGP)
    compact_and_copy_vec_at(dd->ncg_home,move,cgindex,
			    nvec,vec++,state->cg_p,comm,bCompact);
  
  if (bCompact) {
    compact_ind(dd->ncg_home,move,
		dd->index_gl,dd->cgindex,
		dd->gatindex,dd->ga2la,
		fr->cginfo);
  } else {
    clear_and_mark_ind(dd->ncg_home,move,
		       dd->cgindex,dd->gatindex,
		       dd->ga2la,fr->ns.grid->cell_index);
  }

  ncg_stay_home = home_pos_cg;
  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    ncg_recv = 0;
    nat_recv = 0;
    nvr      = 0;
    for(dir=0; dir<(dd->nc[dim]==2 ? 1 : 2); dir++) {
      cdd = d*2 + dir;
      /* Communicate the cg and atom counts */
      sbuf[0] = ncg[cdd];
      sbuf[1] = nat[cdd];
      if (debug)
	fprintf(debug,"Sending ddim %d dir %d: ncg %d nat %d\n",
		d,dir,sbuf[0],sbuf[1]);
      dd_sendrecv_int(dd, d, dir, sbuf, 2, rbuf, 2);

      if ((ncg_recv+rbuf[0])*DD_CGIBS > comm->nalloc_int) {
	comm->nalloc_int = over_alloc_dd((ncg_recv+rbuf[0])*DD_CGIBS);
	srenew(comm->buf_int,comm->nalloc_int);
      }

      /* Communicate the charge group indices, sizes and flags */
      dd_sendrecv_int(dd, d, dir,
		      comm->cggl_flag[cdd], sbuf[0]*DD_CGIBS,
		      comm->buf_int+ncg_recv*DD_CGIBS, rbuf[0]*DD_CGIBS);
      
      nvs = ncg[cdd] + nat[cdd]*nvec;
      i   = rbuf[0]  + rbuf[1] *nvec;
      if (nvr+i > comm->nalloc_vr) {
	comm->nalloc_vr = over_alloc_dd(nvr+i);
	srenew(comm->buf_vr,comm->nalloc_vr);
      }

      /* Communicate cgcm and state */
      dd_sendrecv_rvec(dd, d, dir,
		       comm->cgcm_state[cdd], nvs,
		       comm->buf_vr+nvr, i);
      ncg_recv += rbuf[0];
      nat_recv += rbuf[1];
      nvr      += i;
    }

    /* Process the received charge groups */
    buf_pos = 0;
    for(cg=0; cg<ncg_recv; cg++) {
      flag = comm->buf_int[cg*DD_CGIBS+1];
      mc = -1;
      if (d < dd->ndim-1) {
	/* Check which direction this cg should go */
	for(d2=d+1; (d2<dd->ndim && mc==-1); d2++) {
	  if (dd->bGridJump) {
	    /* The cell boundaries for dimension d2 are not equal
	     * for each cell row of the lower dimension(s),
	     * therefore we might need to redetermine where
	     * this cg should go.
	     */
	    dim2 = dd->dim[d2];
	    /* If this cg crosses the box boundary in dimension d2
	     * we can use the communicated flag, so we do not
	     * have to worry about pbc.
	     */
	    if (!((dd->ci[dim2] == dd->nc[dim2]-1 &&
		   (flag & DD_FLAG_FW(d2))) ||
		  (dd->ci[dim2] == 0 &&
		   (flag & DD_FLAG_BW(d2))))) {
	      /* Clear the two flags for this dimension */
	      flag &= ~(DD_FLAG_FW(d2) | DD_FLAG_BW(d2));
	      /* Determine the location of this cg in lattice coordinates */
	      pos_d = comm->buf_vr[buf_pos][dim2];
	      if (tric_dir[dim2])
		for(d3=dim2+1; d3<DIM; d3++)
		  pos_d += comm->buf_vr[buf_pos][d3]*tcm[d3][dim2];
	      if (pos_d >= cell_x1[dim2]) {
		flag |= DD_FLAG_FW(d2);
	      } else if (pos_d < cell_x0[dim2]) {
		flag |= DD_FLAG_BW(d2);
	      }
	      comm->buf_int[cg*DD_CGIBS+1] = flag;
	    }
	  }
	  /* Set to which neighboring cell this cg should go */
	  if (flag & DD_FLAG_FW(d2)) {
	    mc = d2*2;
	  } else if (flag & DD_FLAG_BW(d2)) {
	    if (dd->nc[dd->dim[d2]] > 2)
	      mc = d2*2+1;
	    else
	      mc = d2*2;
	  }
	}
      }
      
      nrcg = flag & DD_FLAG_NRCG;
      if (mc == -1) {
	if (home_pos_cg+1 > dd->cg_nalloc) {
	  dd->cg_nalloc = over_alloc_dd(home_pos_cg+1);
	  srenew(dd->index_gl,dd->cg_nalloc);
	  srenew(dd->cgindex,dd->cg_nalloc+1);
	}
	/* Set the global charge group index and size */
	dd->index_gl[home_pos_cg] = comm->buf_int[cg*DD_CGIBS];
	dd->cgindex[home_pos_cg+1] = dd->cgindex[home_pos_cg] + nrcg;
	/* Copy the state from the buffer */
	if (home_pos_cg >= fr->cg_nalloc) {
	  dd_realloc_fr_cg(fr,home_pos_cg+1);
	  cg_cm = fr->cg_cm;
	}
	copy_rvec(comm->buf_vr[buf_pos++],cg_cm[home_pos_cg]);
	if (home_pos_at+nrcg > state->nalloc)
	  dd_realloc_state(state,f,buf,home_pos_at+nrcg);
	for(i=0; i<nrcg; i++)
	  copy_rvec(comm->buf_vr[buf_pos++],state->x[home_pos_at+i]);
	if (bV) {
	  for(i=0; i<nrcg; i++)
	    copy_rvec(comm->buf_vr[buf_pos++],state->v[home_pos_at+i]);
	}
	if (bSDX) {
	  for(i=0; i<nrcg; i++)
	    copy_rvec(comm->buf_vr[buf_pos++],state->sd_X[home_pos_at+i]);
	}
	if (bCGP) {
	  for(i=0; i<nrcg; i++)
	    copy_rvec(comm->buf_vr[buf_pos++],state->cg_p[home_pos_at+i]);
	}
	home_pos_cg += 1;
	home_pos_at += nrcg;
      } else {
	/* Reallocate the buffers if necessary  */
	if (ncg[mc]+1 > comm->cggl_flag_nalloc[mc]) {
	  comm->cggl_flag_nalloc[mc] = over_alloc_dd(ncg[mc]+1);
	  srenew(comm->cggl_flag[mc],comm->cggl_flag_nalloc[mc]*DD_CGIBS);
	}
	nvr = ncg[mc] + nat[mc]*nvec;
	if (nvr + 1 + nrcg*nvec > comm->cgcm_state_nalloc[mc]) {
	  comm->cgcm_state_nalloc[mc] = over_alloc_dd(nvr + 1 + nrcg*nvec);
	  srenew(comm->cgcm_state[mc],comm->cgcm_state_nalloc[mc]);
	}
	/* Copy from the receive to the send buffers */
	memcpy(comm->cggl_flag[mc] + ncg[mc]*DD_CGIBS,
	       comm->buf_int + cg*DD_CGIBS,
	       DD_CGIBS*sizeof(int));
	memcpy(comm->cgcm_state[mc][nvr],
	       comm->buf_vr[buf_pos],
	       (1+nrcg*nvec)*sizeof(rvec));
	buf_pos += 1 + nrcg*nvec;
	ncg[mc] += 1;
	nat[mc] += nrcg;
      }
    }
  }

  /* Clear the local indices, except for the home cell.
   * The home cell indices were updated and cleaned in compact_ind.
   */
  clear_dd_indices(dd,dd->nat_home);

  dd->ncg_home = home_pos_cg;
  dd->nat_home = home_pos_at;

  dd->bMasterHasAllCG = FALSE;

  if (debug)
    fprintf(debug,"Finished repartitioning\n");

  return ncg_stay_home;
}

void dd_cycles_add(gmx_domdec_t *dd,float cycles,int ddCycl)
{
  dd->comm->cycl[ddCycl] += cycles;
  dd->comm->cycl_n[ddCycl]++;
}

static double force_flop_count(t_nrnb *nrnb)
{
  int i;
  double sum;

  sum = 0;
  for(i=eNR_NBKERNEL010; i<=eNR_NB14; i++)
    sum += nrnb->n[i]*cost_nrnb(i);
  for(i=eNR_BONDS; i<=eNR_WALLS; i++)
    sum += nrnb->n[i]*cost_nrnb(i);

  return sum;
}

void dd_force_flop_start(gmx_domdec_t *dd,t_nrnb *nrnb)
{
  if (dd->comm->eFlop) {
    dd->comm->flop -= force_flop_count(nrnb);
  }
}
void dd_force_flop_stop(gmx_domdec_t *dd,t_nrnb *nrnb)
{
  if (dd->comm->eFlop) {
    dd->comm->flop += force_flop_count(nrnb);
    dd->comm->flop_n++;
  }
}  

static void clear_dd_cycle_counts(gmx_domdec_t *dd)
{
  int i;
  
  for(i=0; i<ddCyclNr; i++) {
    dd->comm->cycl[i] = 0;
    dd->comm->cycl_n[i] = 0;
  }
  dd->comm->flop = 0;
  dd->comm->flop_n = 0;
}

static void get_load_distribution(gmx_domdec_t *dd,gmx_wallcycle_t wcycle)
{
#ifdef GMX_MPI
  gmx_domdec_comm_t *comm;
  gmx_domdec_load_t *load;
  gmx_domdec_root_t *root=NULL;
  int  d,dim,cid,i,pos;
  float cell_frac=0,sbuf[DD_NLOAD_MAX];
  bool bSepPME;

  if (debug)
    fprintf(debug,"get_load_distribution start\n");

  comm = dd->comm;
  
  bSepPME = (dd->pme_nodeid >= 0);
  
  for(d=dd->ndim-1; d>=0; d--) {
    dim = dd->dim[d];
    /* Check if we participate in the communication in this dimension */
    if (d == dd->ndim-1 || 
	(dd->ci[dd->dim[d+1]]==0 && dd->ci[dd->dim[dd->ndim-1]]==0)) {
      load = &comm->load[d];
      if (dd->bGridJump)
	cell_frac = comm->cell_f1[d] - comm->cell_f0[d];
      pos = 0;
      if (d == dd->ndim-1) {
	sbuf[pos++] = dd_force_load(comm);
	sbuf[pos++] = sbuf[0];
	if (dd->bGridJump) {
	  sbuf[pos++] = sbuf[0];
	  sbuf[pos++] = cell_frac;
	  if (d > 0) {
	    sbuf[pos++] = comm->cell_f_max0[d];
	    sbuf[pos++] = comm->cell_f_min1[d];
	  }
	}
	if (bSepPME) {
	  sbuf[pos++] = comm->cycl[ddCyclPPduringPME];
	  sbuf[pos++] = comm->cycl[ddCyclPME];
	}
      } else {
	sbuf[pos++] = comm->load[d+1].sum;
	sbuf[pos++] = comm->load[d+1].max;
	if (dd->bGridJump) {
	  sbuf[pos++] = comm->load[d+1].sum_m;
	  sbuf[pos++] = comm->load[d+1].cvol_min*cell_frac;
	  sbuf[pos++] = comm->load[d+1].flags;
	  if (d > 0) {
	    sbuf[pos++] = comm->cell_f_max0[d];
	    sbuf[pos++] = comm->cell_f_min1[d];
	  }
	}
	if (bSepPME) {
	  sbuf[pos++] = comm->load[d+1].mdf;
	  sbuf[pos++] = comm->load[d+1].pme;
	}
      }
      load->nload = pos;
      /* Communicate a row in DD direction d.
       * The communicators are setup such that the root always has rank 0.
       */
      MPI_Gather(sbuf      ,load->nload*sizeof(float),MPI_BYTE,
		 load->load,load->nload*sizeof(float),MPI_BYTE,
		 0,comm->mpi_comm_load[d]);
      if (dd->ci[dim] == dd->master_ci[dim]) {
	/* We are the root, process this row */
	if (dd->bDynLoadBal)
	  root = comm->root[d];

	load->sum = 0;
	load->max = 0;
	load->sum_m = 0;
	load->cvol_min = 1;
	load->flags = 0;
	load->mdf = 0;
	load->pme = 0;
	pos = 0;
	for(i=0; i<dd->nc[dim]; i++) {
	  load->sum += load->load[pos++];
	  load->max = max(load->max,load->load[pos]);
	  pos++;
	  if (dd->bGridJump) {
	    if (root->bLimited)
	      /* This direction could not be load balanced properly,
	       * therefore we need to use the maximum iso the average load.
	       */
	      load->sum_m = max(load->sum_m,load->load[pos]);
	    else
	      load->sum_m += load->load[pos];
	    pos++;
	    load->cvol_min = min(load->cvol_min,load->load[pos]);
	    pos++;
	    if (d < dd->ndim-1)
	      load->flags = (int)(load->load[pos++] + 0.5);
	    if (d > 0) {
	      root->cell_f_max0[i] = load->load[pos++];
	      root->cell_f_min1[i] = load->load[pos++];
	    }
	  }
	  if (bSepPME) {
	    load->mdf = max(load->mdf,load->load[pos]);
	    pos++;
	    load->pme = max(load->pme,load->load[pos]);
	    pos++;
	  }
	}
	if (dd->bDynLoadBal && root->bLimited) {
	  load->sum_m *= dd->nc[dim];
	  load->flags |= (1<<d);
	}
      }
    }
  }

  if (DDMASTER(dd)) {
    comm->nload      += dd_load_count(comm);
    comm->load_step  += comm->cycl[ddCyclStep];
    comm->load_sum   += comm->load[0].sum;
    comm->load_max   += comm->load[0].max;
    if (dd->bDynLoadBal) {
      for(d=0; d<dd->ndim; d++) {
	if (comm->load[0].flags & (1<<d))
	  comm->load_lim[d]++;
      }
    }
    if (bSepPME) {
      comm->load_mdf += comm->load[0].mdf;
      comm->load_pme += comm->load[0].pme;
    }
  }
#endif

  if (debug)
    fprintf(debug,"get_load_distribution finished\n");
}

static void print_dd_load_av(FILE *fplog,gmx_domdec_t *dd)
{
  char  buf[STRLEN];
  int   npp,npme,nnodes,d,limp;
  float imbal,pme_f_ratio,lossf,lossp=0;
  bool  bLim;
  gmx_domdec_comm_t *comm;

  comm = dd->comm;
  if (DDMASTER(dd) && comm->nload > 0) {
    npp    = dd->nnodes;
    npme   = (dd->pme_nodeid >= 0) ? comm->npmenodes : 0;
    nnodes = npp + npme;
    imbal = comm->load_max*npp/comm->load_sum - 1;
    lossf = (comm->load_max*npp - comm->load_sum)/(comm->load_step*nnodes);
    sprintf(buf,"Average load imbalance: %.1f %%\n",imbal*100);
    fprintf(fplog,buf);
    fprintf(stderr,"\n");
    fprintf(stderr,buf);
    sprintf(buf,"Part of the total run time spent waiting due to load imbalance: %.1f %%\n",lossf*100);
    fprintf(fplog,buf);
    fprintf(stderr,buf);
    bLim = FALSE;
    if (dd->bDynLoadBal) {
      sprintf(buf,"Steps where the load balancing was limited by -rdd, -rcon and/or -dds:");
      for(d=0; d<dd->ndim; d++) {
	limp = (200*comm->load_lim[d]+1)/(2*comm->nload);
	sprintf(buf+strlen(buf)," %c %d %%",dim2char(dd->dim[d]),limp);
	if (limp >= 50)
	  bLim = TRUE;
      }
      sprintf(buf+strlen(buf),"\n");
      fprintf(fplog,buf);
      fprintf(stderr,buf);
    }
    if (npme > 0) {
      pme_f_ratio = comm->load_pme/comm->load_mdf;
      lossp = (comm->load_pme -comm->load_mdf)/comm->load_step;
      if (lossp <= 0)
	lossp *= (float)npme/(float)nnodes;
      else
	lossp *= (float)npp/(float)nnodes;
      sprintf(buf,"Average PME mesh/force load: %5.3f\n",pme_f_ratio);
      fprintf(fplog,"%s",buf);
      fprintf(stderr,"%s",buf);
      sprintf(buf,"Part of the total run time spent waiting due to PP/PME imbalance: %.1f %%\n",fabs(lossp)*100);
      fprintf(fplog,"%s",buf);
      fprintf(stderr,"%s",buf);
    }
    fprintf(fplog,"\n");
    fprintf(stderr,"\n");

    if (lossf >= 5) {
      sprintf(buf,
	      "NOTE: %.1f %% performance was lost due to load imbalance\n"
	      "      in the domain decomposition.\n",lossf*100);
      if (!dd->bDynLoadBal) {
	sprintf(buf+strlen(buf),"      You might want to use dynamic load balancing (option -dlb.)\n");
      } else if (bLim) {
	sprintf(buf+strlen(buf),"      You might want to decrease the cell size limit (options -rdd, -rcon and/or -dds).\n");
      }
      fprintf(fplog,"%s\n",buf);
      fprintf(stderr,"%s\n",buf);
    }
    if (npme > 0 && fabs(lossp) >= 0.05) {
      sprintf(buf,
	      "NOTE: %.1f %% performance was lost because the PME nodes\n"
	      "      had %s work to do than the PP nodes.\n"
	      "      You might want to %s the number of PME nodes\n"
	      "      or %s the cut-off and the grid spacing.\n",
	      fabs(lossp*100),
	      (lossp < 0) ? "less"     : "more",
	      (lossp < 0) ? "decrease" : "increase",
	      (lossp < 0) ? "decrease" : "increase");
      fprintf(fplog,"%s\n",buf);
      fprintf(stderr,"%s\n",buf);
    }
  }
}

static float dd_vol_min(gmx_domdec_t *dd)
{
  return dd->comm->load[0].cvol_min*dd->nnodes;
}

static bool dd_load_flags(gmx_domdec_t *dd)
{
  return dd->comm->load[0].flags;
}

static float dd_f_imbal(gmx_domdec_t *dd)
{
  return dd->comm->load[0].max*dd->nnodes/dd->comm->load[0].sum - 1;
}

static float dd_pme_f_ratio(gmx_domdec_t *dd)
{
  return dd->comm->load[0].pme/dd->comm->load[0].mdf;
}

static void dd_print_load(FILE *fplog,gmx_domdec_t *dd,int step)
{
  int flags,d;

  flags = dd_load_flags(dd);
  if (flags) {
    fprintf(fplog,
	    "DD  load balancing is limited by minimum cell size in dimension");
    for(d=0; d<dd->ndim; d++)
      if (flags & (1<<d))
	fprintf(fplog," %c",dim2char(dd->dim[d]));
    fprintf(fplog,"\n");
  }
  fprintf(fplog,"DD  step %d",step);
  if (dd->bDynLoadBal)
    fprintf(fplog,"  vol min/aver %5.3f%c",dd_vol_min(dd),flags ? '!' : ' ');
  fprintf(fplog," load imb.: force %4.1f%%",dd_f_imbal(dd)*100);
  if (dd->comm->cycl_n[ddCyclPME])
    fprintf(fplog,"  pme mesh/force %5.3f",dd_pme_f_ratio(dd));
  fprintf(fplog,"\n\n");
}

static void dd_print_load_verbose(gmx_domdec_t *dd)
{
  if (dd->bDynLoadBal)
    fprintf(stderr,"vol %4.2f%c ",
	    dd_vol_min(dd),dd_load_flags(dd) ? '!' : ' ');
  fprintf(stderr,"imb F %2d%% ",(int)(dd_f_imbal(dd)*100+0.5));
  if (dd->comm->cycl_n[ddCyclPME])
    fprintf(stderr,"pme/F %4.2f ",dd_pme_f_ratio(dd));
}


#ifdef GMX_MPI
static void make_load_communicator(gmx_domdec_t *dd,MPI_Group g_all,
				   int dim_ind,ivec loc)
{
  MPI_Group g_row;
  MPI_Comm  c_row;
  int  dim,i,*rank;
  ivec loc_c;
  gmx_domdec_root_t *root;

  dim = dd->dim[dim_ind];
  copy_ivec(loc,loc_c);
  snew(rank,dd->nc[dim]);
  for(i=0; i<dd->nc[dim]; i++) {
    loc_c[dim] = i;
    rank[i] = dd_index(dd->nc,loc_c);
  }
  /* Here we create a new group, that does not necessarily
   * include our process. But MPI_Comm_create needs to be
   * called by all the processes in the original communicator.
   * Calling MPI_Group_free afterwards gives errors, so I assume
   * also the group is needed by all processes. (B. Hess)
   */
  MPI_Group_incl(g_all,dd->nc[dim],rank,&g_row);
  MPI_Comm_create(dd->comm->all,g_row,&c_row);
  if (c_row != MPI_COMM_NULL) {
    /* This process is part of the group */
    dd->comm->mpi_comm_load[dim_ind] = c_row;
    if (dd->bGridJump) {
      if (dd->ci[dim] == dd->master_ci[dim]) {
	/* This is the root process of this row */
	snew(dd->comm->root[dim_ind],1);
	root = dd->comm->root[dim_ind];
	snew(root->cell_f,DD_CELL_F_SIZE(dd,dim_ind));
	snew(root->old_cell_f,dd->nc[dim]+1);
	snew(root->cell_size,dd->nc[dim]);
	snew(root->bCellMin,dd->nc[dim]);
	if (dim_ind > 0) {
	  snew(root->cell_f_max0,dd->nc[dim]);
	  snew(root->cell_f_min1,dd->nc[dim]);
	  snew(root->bound_min,dd->nc[dim]);
	  snew(root->bound_max,dd->nc[dim]);
	}
      } else {
	/* This is not a root process, we only need to receive cell_f */
	snew(dd->comm->cell_f_row,DD_CELL_F_SIZE(dd,dim_ind));
      }
    }
    if (dd->ci[dim] == dd->master_ci[dim])
      snew(dd->comm->load[dim_ind].load,dd->nc[dim]*DD_NLOAD_MAX);
  }
  sfree(rank);
}
#endif

static void make_load_communicators(gmx_domdec_t *dd)
{
#ifdef GMX_MPI
  MPI_Group g_all;
  int  dim0,dim1,i,j;
  ivec loc;

  if (debug)
    fprintf(debug,"Making load communicators\n");

  MPI_Comm_group(dd->comm->all,&g_all);
  
  snew(dd->comm->load,dd->ndim);
  snew(dd->comm->mpi_comm_load,dd->ndim);
  
  clear_ivec(loc);
  make_load_communicator(dd,g_all,0,loc);
  if (dd->ndim > 1) {
    dim0 = dd->dim[0];
    for(i=0; i<dd->nc[dim0]; i++) {
      loc[dim0] = i;
      make_load_communicator(dd,g_all,1,loc);
    }
  }
  if (dd->ndim > 2) {
    dim0 = dd->dim[0];
    for(i=0; i<dd->nc[dim0]; i++) {
      loc[dim0] = i;
      dim1 = dd->dim[1];
      for(j=0; j<dd->nc[dim1]; j++) {
	  loc[dim1] = j;
	  make_load_communicator(dd,g_all,2,loc);
      }
    }
  }

  MPI_Group_free(&g_all);

  if (debug)
    fprintf(debug,"Finished making load communicators\n");
#endif
}

void setup_dd_grid(FILE *fplog,gmx_domdec_t *dd)
{
  bool bZYX;
  int  d,dim,i,j,m;
  ivec tmp,s;
  int  ncell,ncellp;
  ivec dd_cp[DD_MAXICELL];
  gmx_domdec_ns_ranges_t *icell;

  for(d=0; d<dd->ndim; d++) {
    dim = dd->dim[d];
    copy_ivec(dd->ci,tmp);
    tmp[dim] = (tmp[dim] + 1) % dd->nc[dim];
    dd->neighbor[d][0] = ddcoord2ddnodeid(dd,tmp);
    copy_ivec(dd->ci,tmp);
    tmp[dim] = (tmp[dim] - 1 + dd->nc[dim]) % dd->nc[dim];
    dd->neighbor[d][1] = ddcoord2ddnodeid(dd,tmp);
    if (debug)
      fprintf(debug,"DD rank %d neighbor ranks in dir %d are + %d - %d\n",
	      dd->rank,dim,
	      dd->neighbor[d][0],
	      dd->neighbor[d][1]);
  }
  
  if (DDMASTER(dd))
    fprintf(stderr,"Making %dD domain decomposition %d x %d x %d\n",
	    dd->ndim,dd->nc[XX],dd->nc[YY],dd->nc[ZZ]);
  if (fplog) {
    fprintf(fplog,"\nMaking %dD domain decomposition grid %d x %d x %d, home cell index %d %d %d\n\n",
	    dd->ndim,
	    dd->nc[XX],dd->nc[YY],dd->nc[ZZ],
	    dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
  }
  switch (dd->ndim) {
  case 3:
    ncell  = dd_c3n;
    ncellp = dd_cp3n;
    for(i=0; i<ncellp; i++)
      copy_ivec(dd_cp3[i],dd_cp[i]);
    break;
  case 2:
    ncell  = dd_c2n;
    ncellp = dd_cp2n;
    for(i=0; i<ncellp; i++)
      copy_ivec(dd_cp2[i],dd_cp[i]);
    break;
  case 1:
    ncell  = dd_c1n;
    ncellp = dd_cp1n;
    for(i=0; i<ncellp; i++)
      copy_ivec(dd_cp1[i],dd_cp[i]);
    break;
  default:
    gmx_fatal(FARGS,"Can only do 1, 2 or 3D domain decomposition");
    ncell = 0;
    ncellp = 0;
  }
    
  for(i=0; i<ncell; i++) {
    m = 0;
    clear_ivec(dd->shift[i]);
    for(d=0; d<dd->ndim; d++)
      dd->shift[i][dd->dim[d]] = dd_co[i][m++];
  }

  dd->ncell  = ncell;
  for(i=0; i<ncell; i++) {
    for(d=0; d<DIM; d++) {
      s[d] = dd->ci[d] - dd->shift[i][d];
      if (s[d] < 0)
	s[d] += dd->nc[d];
      else if (s[d] >= dd->nc[d])
	s[d] -= dd->nc[d];
    }
  }
  dd->nicell = ncellp;
  for(i=0; i<dd->nicell; i++) {
    if (dd_cp[i][0] != i)
      gmx_fatal(FARGS,"Internal inconsistency in the dd grid setup");
    icell = &dd->icell[i];
    icell->j0 = dd_cp[i][1];
    icell->j1 = dd_cp[i][2];
    for(dim=0; dim<DIM; dim++) {
      if (dd->nc[dim] == 1) {
	/* All shifts should be allowed */
	icell->shift0[dim] = -1;
	icell->shift1[dim] = 1;
      } else {
	/*
	icell->shift0[d] = 0;
	icell->shift1[d] = 0;
	for(j=icell->j0; j<icell->j1; j++) {
	  if (dd->shift[j][d] > dd->shift[i][d])
	    icell->shift0[d] = -1;
	  if (dd->shift[j][d] < dd->shift[i][d])
	    icell->shift1[d] = 1;
	}
	*/
	
	int shift_diff;
	
	/* Assume the shift are not more than 1 cell */
	icell->shift0[dim] = 1;
	icell->shift1[dim] = -1;
	for(j=icell->j0; j<icell->j1; j++) {
	  shift_diff = dd->shift[j][dim] - dd->shift[i][dim];
	  if (shift_diff < icell->shift0[dim])
	    icell->shift0[dim] = shift_diff;
	  if (shift_diff > icell->shift1[dim])
	    icell->shift1[dim] = shift_diff;
	}
      }
    }
  }

  if (dd->bGridJump)
    snew(dd->comm->root,dd->ndim);

  if (dd->comm->bRecordLoad)
    make_load_communicators(dd);
}

static void make_pp_communicator(FILE *fplog,t_commrec *cr,int reorder)
{
  gmx_domdec_t *dd;
  gmx_domdec_comm_t *comm;
  int  i,rank,*buf;
  ivec periods;
#ifdef GMX_MPI
  MPI_Comm comm_cart;
#endif

  dd = cr->dd;
  comm = dd->comm;

#ifdef GMX_MPI
  if (comm->bCartesianPP) {
    /* Set up cartesian communication for the particle-particle part */
    if (fplog)
      fprintf(fplog,"Will use a Cartesian communicator: %d x %d x %d\n",
	      dd->nc[XX],dd->nc[YY],dd->nc[ZZ]);
    
    for(i=0; i<DIM; i++)
      periods[i] = TRUE;
    MPI_Cart_create(cr->mpi_comm_mygroup,DIM,dd->nc,periods,reorder,
		    &comm_cart);
    /* We overwrite the old communicator with the new cartesian one */
    cr->mpi_comm_mygroup = comm_cart;
  }
  
  comm->all = cr->mpi_comm_mygroup;
  MPI_Comm_rank(comm->all,&dd->rank);
  
  if (comm->bCartesianPP_PME) {
    /* Since we want to use the original cartesian setup for sim,
     * and not the one after split, we need to make an index.
     */
    snew(comm->ddindex2ddnodeid,dd->nnodes);
    comm->ddindex2ddnodeid[dd_index(dd->nc,dd->ci)] = dd->rank;
    gmx_sumi(dd->nnodes,comm->ddindex2ddnodeid,cr);
    /* Get the rank of the DD master,
     * above we made sure that the master node is a PP node.
     */
    if (MASTER(cr))
      rank = dd->rank;
    else
      rank = 0;
    MPI_Allreduce(&rank,&dd->masterrank,1,MPI_INT,MPI_SUM,comm->all);
  } else if (comm->bCartesianPP) {
    if (cr->npmenodes == 0) {
      /* The PP communicator is also the communicator for this simulation */
      cr->mpi_comm_mysim = cr->mpi_comm_mygroup;
    }
    cr->nodeid = dd->rank;

    MPI_Cart_coords(comm->all,dd->rank,DIM,dd->ci);
    
    /* We need to make an index to go from the coordinates
     * to the nodeid of this simulation.
     */
    snew(comm->ddindex2simnodeid,dd->nnodes);
    snew(buf,dd->nnodes);
    if (cr->duty & DUTY_PP)
      buf[dd_index(dd->nc,dd->ci)] = cr->sim_nodeid;
    /* Communicate the ddindex to simulation nodeid index */
    MPI_Allreduce(buf,comm->ddindex2simnodeid,dd->nnodes,MPI_INT,MPI_SUM,
		  cr->mpi_comm_mysim);
    sfree(buf);
    
    /* Determine the master coordinates and rank.
     * The DD master should be the same node as the master of this sim.
     */
    for(i=0; i<dd->nnodes; i++) {
      if (comm->ddindex2simnodeid[i] == 0) {
	ddindex2xyz(dd->nc,i,dd->master_ci);
	MPI_Cart_rank(comm->all,dd->master_ci,&dd->masterrank);
      }
    }
    if (debug)
      fprintf(debug,"The master rank is %d\n",dd->masterrank);
  } else {
    /* No Cartesian communicators */
    /* We use the rank in dd->comm->all as DD index */
    ddindex2xyz(dd->nc,dd->rank,dd->ci);
    /* The simulation master nodeid is 0, so the DD master rank is also 0 */
    dd->masterrank = 0;
    clear_ivec(dd->master_ci);
  }
#endif
  
  if (fplog)
    fprintf(fplog,"Domain decomposition nodeid %d, coordinates %d %d %d\n\n",
	    dd->rank,dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
  if (debug)
    fprintf(debug,"Domain decomposition nodeid %d, coordinates %d %d %d\n\n",
	    dd->rank,dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
}

static void receive_ddindex2simnodeid(t_commrec *cr)
{
  gmx_domdec_t *dd;
  
  gmx_domdec_comm_t *comm;
  int  *buf;

  dd = cr->dd;
  comm = dd->comm;

  if (!comm->bCartesianPP_PME && comm->bCartesianPP) {
    snew(comm->ddindex2simnodeid,dd->nnodes);
    snew(buf,dd->nnodes);
    if (cr->duty & DUTY_PP)
      buf[dd_index(dd->nc,dd->ci)] = cr->sim_nodeid;
#ifdef GMX_MPI
    /* Communicate the ddindex to simulation nodeid index */
    MPI_Allreduce(buf,comm->ddindex2simnodeid,dd->nnodes,MPI_INT,MPI_SUM,
		  cr->mpi_comm_mysim);
#endif
    sfree(buf);
  }
}

static void split_communicator(FILE *fplog,t_commrec *cr,int dd_node_order,
			       int reorder)
{
  gmx_domdec_t *dd;
  gmx_domdec_comm_t *comm;
  int  i,rank;
  bool bDiv[DIM];
  ivec periods;
#ifdef GMX_MPI
  MPI_Comm comm_cart;
#endif

  dd = cr->dd;
  comm = dd->comm;

  if (comm->bCartesianPP) {
    for(i=1; i<DIM; i++)
      bDiv[i] = ((cr->npmenodes*dd->nc[i]) % (dd->nnodes) == 0);
    if (bDiv[YY] || bDiv[ZZ]) {
      comm->bCartesianPP_PME = TRUE;
      /* We choose the direction that provides the thinnest slab
       * of PME only nodes as this will have the least effect
       * on the PP communication.
       * But for the PME communication the opposite might be better.
       */
      if (bDiv[YY] && (!bDiv[ZZ] || dd->nc[YY] <= dd->nc[ZZ])) {
	comm->cartpmedim = YY;
      } else {
	comm->cartpmedim = ZZ;
      }
      comm->ntot[comm->cartpmedim]
	+= (cr->npmenodes*dd->nc[comm->cartpmedim])/dd->nnodes;
    } else if (fplog) {
      fprintf(fplog,"#pmenodes (%d) is not a multiple of nx*ny (%d*%d) or nx*nz (%d*%d)\n",cr->npmenodes,dd->nc[XX],dd->nc[YY],dd->nc[XX],dd->nc[ZZ]);
      fprintf(fplog,
	      "Will not use a Cartesian communicator for PP <-> PME\n\n");
    }
  }

#ifdef GMX_MPI
  if (comm->bCartesianPP_PME) {
    if (fplog)
      fprintf(fplog,"Will use a Cartesian communicator for PP <-> PME: %d x %d x %d\n",comm->ntot[XX],comm->ntot[YY],comm->ntot[ZZ]);

    for(i=0; i<DIM; i++)
      periods[i] = TRUE;
    MPI_Cart_create(cr->mpi_comm_mysim,DIM,comm->ntot,periods,reorder,
		    &comm_cart);

    MPI_Comm_rank(comm_cart,&rank);
    if (MASTERNODE(cr) && rank != 0)
      gmx_fatal(FARGS,"MPI rank 0 was renumbered by MPI_Cart_create, we do not allow this");

    /* With this assigment we loose the link to the original communicator
     * which will usually be MPI_COMM_WORLD, unless have multisim.
     */
    cr->mpi_comm_mysim = comm_cart;
    cr->sim_nodeid = rank;

    MPI_Cart_coords(cr->mpi_comm_mysim,cr->sim_nodeid,DIM,dd->ci);

    if (fplog)
      fprintf(fplog,"Cartesian nodeid %d, coordinates %d %d %d\n\n",
	      cr->sim_nodeid,dd->ci[XX],dd->ci[YY],dd->ci[ZZ]);
    
    if (dd->ci[comm->cartpmedim] < dd->nc[comm->cartpmedim])
      cr->duty = DUTY_PP;
    if (cr->npmenodes == 0 ||
	dd->ci[comm->cartpmedim] >= dd->nc[comm->cartpmedim])
      cr->duty = DUTY_PME;

    /* Split the sim communicator into PP and PME only nodes */
    MPI_Comm_split(cr->mpi_comm_mysim,
		   cr->duty,
		   dd_index(comm->ntot,dd->ci),
		   &cr->mpi_comm_mygroup);
  } else {
    switch (dd_node_order) {
    case ddnoPP_PME:
      if (fplog)
	fprintf(fplog,"Order of the nodes: PP first, PME last\n");
      break;
    case ddnoINTERLEAVE:
      /* Interleave the PP-only and PME-only nodes,
       * as on clusters with dual-core machines this will double
       * the communication bandwidth of the PME processes
       * and thus speed up the PP <-> PME and inter PME communication.
       */
      if (fplog)
	fprintf(fplog,"Interleaving PP and PME nodes\n");
      comm->pmenodes = dd_pmenodes(cr);
      break;
    case ddnoCARTESIAN:
      break;
    default:
      gmx_fatal(FARGS,"Unknown dd_node_order=%d",dd_node_order);
    }
    
    if (dd_simnode2pmenode(cr,cr->sim_nodeid) == -1)
      cr->duty = DUTY_PME;
    else
      cr->duty = DUTY_PP;
    
    /* Split the sim communicator into PP and PME only nodes */
    MPI_Comm_split(cr->mpi_comm_mysim,
		   cr->duty,
		   cr->nodeid,
		   &cr->mpi_comm_mygroup);
    MPI_Comm_rank(cr->mpi_comm_mygroup,&cr->nodeid);
  }
#endif

  if (fplog)
    fprintf(fplog,"This is a %s only node\n\n",
	    (cr->duty & DUTY_PP) ? "particle-particle" : "PME-mesh");
}

void make_dd_communicators(FILE *fplog,t_commrec *cr,int dd_node_order)
{
  gmx_domdec_t *dd;
  gmx_domdec_comm_t *comm;
  int CartReorder;

  dd = cr->dd;
  comm = dd->comm;

  copy_ivec(dd->nc,comm->ntot);
  
  comm->bCartesianPP = (dd_node_order == ddnoCARTESIAN);
  comm->bCartesianPP_PME = FALSE;

  CartReorder = (getenv("GMX_NO_CART_REORDER") == NULL);

  if (cr->npmenodes > 0) {
    /* Split the communicator into a PP and PME part */
    split_communicator(fplog,cr,dd_node_order,CartReorder);
  } else {
    /* All nodes do PP and PME */
#ifdef GMX_MPI    
    /* We do not require separate communicators */
    cr->mpi_comm_mygroup = cr->mpi_comm_mysim;
#endif
  }
  
  if (cr->duty & DUTY_PP) {
    /* Copy or make a new PP communicator */
    make_pp_communicator(fplog,cr,CartReorder);
  } else {
    receive_ddindex2simnodeid(cr);
  }
  
  if (!(cr->duty & DUTY_PME)) {
    /* Set up the commnuication to our PME node */
    dd->pme_nodeid = dd_simnode2pmenode(cr,cr->sim_nodeid);
    dd->pme_receive_vir_ener = receive_vir_ener(cr);
    if (debug)
      fprintf(debug,"My pme_nodeid %d receive ener %d\n",
	      dd->pme_nodeid,dd->pme_receive_vir_ener);
  } else {
    dd->pme_nodeid = -1;
  }

  if (DDMASTER(dd) && dd->ma == NULL) {
    snew(dd->ma,1);
  }
}

static real *get_slb_frac(FILE *fplog,char *dir,int nc,char *size_string)
{
  real *slb_frac,tot;
  int  i,n;
  double dbl;

  slb_frac = NULL;
  if (nc > 1 && size_string != NULL) {
    if (fplog)
      fprintf(fplog,"Using static load balancing for the %s direction\n",dir);
    snew(slb_frac,nc);
    tot = 0;
    for (i=0; i<nc; i++) {
      dbl = 0;
      sscanf(size_string,"%lf%n",&dbl,&n);
      if (dbl == 0)
	gmx_fatal(FARGS,"Incorrect or not enough DD cell size entries for direction %s: '%s'",dir,size_string);
      slb_frac[i] = dbl;
      size_string += n;
      tot += slb_frac[i];
    }
    /* Normalize */
    if (fplog)
      fprintf(fplog,"Relative cell sizes:");
    for (i=0; i<nc; i++) {
      slb_frac[i] /= tot;
      if (fplog)
	fprintf(fplog," %5.3f",slb_frac[i]);
    }
    if (fplog)
      fprintf(fplog,"\n");
  }
  
  return slb_frac;
}

static void set_slb_pme_dim_f(gmx_domdec_t *dd)
{
  gmx_domdec_comm_t *comm;
  int i;

  comm = dd->comm;

  if (dd->dim[0] != XX) {
    comm->pme_dim_f = NULL;
    return;
  }

  snew(comm->pme_dim_f,dd->nc[XX]+1);
  comm->pme_dim_f[0] = 0;
  for(i=1; i<dd->nc[XX]; i++) {
    if (comm->slb_frac[XX]) {
      comm->pme_dim_f[i] = comm->pme_dim_f[i-1] + comm->slb_frac[XX][i-1];
    } else {
      comm->pme_dim_f[i] = (real)i/(real)dd->nc[XX];
    }
  }
  comm->pme_dim_f[dd->nc[XX]] = 1;
}

static int factorize(int n,int **fac,int **mfac)
{
  int d,ndiv;

  /* Decompose n in factors */
  snew(*fac,n/2);
  snew(*mfac,n/2);
  d = 2;
  ndiv = 0;
  while (n > 1) {
    while (n % d == 0) {
      if (ndiv == 0 || (*fac)[ndiv-1] != d) {
	ndiv++;
	(*fac)[ndiv-1] = d;
      }
      (*mfac)[ndiv-1]++;
      n /= d;
    }
    d++;
  }

  return ndiv;
}

static int guess_npme(FILE *fplog,t_topology *top,t_inputrec *ir,matrix box,
		      int nnodes)
{
  float ratio;
  int  npme,ndiv,*div,*mdiv,ldiv;

  ratio = pme_load_estimate(top,ir,box);

  if (fplog)
    fprintf(fplog,"Guess for relative PME load: %.2f\n",ratio);

  /* We assume the optimal node ratio is close to the load ratio.
   * The communication load is neglected,
   * but (hopefully) this will balance out between PP and PME.
   */

  /* First try to find npme as a factor of nnodes up to nnodes/3 */
  npme = 1;
  while (npme <= nnodes/3) {
    if ((ir->nkx % npme == 0 && ir->nky % npme == 0) &&
	nnodes % npme == 0 && (double)npme/(double)nnodes > 0.95*ratio) {
      break;
    }
    npme++;
  }
  if (npme > nnodes/3) {
    /* Try any possible number for npme */
    npme = 1;
    while (npme <= nnodes/2) {
      if ((ir->nkx % npme == 0 && ir->nky % npme == 0) &&
	  (double)npme/(double)nnodes > 0.95*ratio) {
	ndiv = factorize(nnodes-npme,&div,&mdiv);
	ldiv = div[ndiv-1];
	sfree(div);
	sfree(mdiv);
	/* Only use this value if nnodes-npme does not have
	 * a large prime factor (5 y, 7 n, 14 n, 15 y).
	 */
	if (ldiv <= 3 + (int)(pow(nnodes-npme,1.0/3.0) + 0.5))
	  break;
      }
      npme++;
    }
  }
  if (npme > nnodes/2) {
    if (ir->nkx % nnodes || ir->nky % nnodes)
      gmx_fatal(FARGS,"Could not find an appropriate numbers of separate PME nodes that is a multiple of the fourier grid x (%d) and y (%d) components.\n"
		"Change the number of processors of the grid dimensions.",
		ir->nkx,ir->nky);
    npme = 0;
  } else {
    if (fplog)
      fprintf(fplog,
	      "Will use %d particle-particle and %d PME only nodes\n"
	      "This is a guess, check the performance at the end of the log file\n",
	      nnodes-npme,npme);
    fprintf(stderr,"\n"
	    "Will use %d particle-particle and %d PME only nodes\n"
	    "This is a guess, check the performance at the end of the log file\n",
	    nnodes-npme,npme);
  }

  return npme;
}

static int lcd(int n1,int n2)
{
  int d,i;
  
  d = 1;
  for(i=2; (i<=n1 && i<=n2); i++) {
    if (n1 % i == 0 && n2 % i == 0)
      d = i;
  }

  return d;
}

static float comm_cost_est(gmx_domdec_t *dd,real limit,
			   matrix box,t_inputrec *ir,float pbcdxr,
			   ivec nc)
{
  int  i,j,k,npp;
  rvec bt,nw;
  float comm_vol,comm_vol_pme,cost_pbcdx;
  /* This is the cost of a pbc_dx call relative to the cost
   * of communicating the coordinate and force of an atom.
   * This will be machine dependent.
   * These factors are for x86 with SMP or Infiniband.
   */
  float pbcdx_rect_fac = 0.1;
  float pbcdx_tric_fac = 0.2;
  
  /* Check if the triclinic requirements are met */
  for(i=0; i<DIM; i++) {
    for(j=i+1; j<DIM; j++) {
      if (box[j][i] != 0) {
	if (nc[j] > 1 && nc[i] == 1)
	  return -1;
      }
    }
  }

  for(i=0; i<DIM; i++) {
    bt[i] = box[i][i]*dd->skew_fac[i];
    nw[i] = nc[i]*dd->comm->cutoff/bt[i];

    if (bt[i] < nc[i]*limit)
      return -1;
  }
    
  /* When two dimensions are (nearly) equal, use more cells
   * for the smallest index, so the decomposition does not
   * depend sensitively on the rounding of the box elements.
   */
  for(i=0; i<DIM; i++) {
    if (dd->comm->npmenodes == 0 || i != XX) {
      for(j=i+1; j<DIM; j++) {
	if (fabs(bt[j] - bt[i]) < 0.01*bt[i] && nc[j] > nc[i])
	  return -1;
      }
    }
  }

  npp = 1;
  comm_vol = 0;
  for(i=0; i<DIM; i++) {
    if (nc[i] > 1) {
      npp *= nc[i];
      comm_vol += nw[i];
      for(j=i+1; j<DIM; j++) {
	if (nc[j] > 1) {
	  comm_vol += nw[i]*nw[j]*M_PI/4;
	  for(k=j+1; k<DIM; k++) {
	    if (nc[k] > 1) {
	      comm_vol += nw[i]*nw[j]*nw[k]*M_PI/6;
	    }
	  }
	}
      }
    }
  }
  /* Normalize of the number of PP nodes */
  comm_vol /= npp;

  /* Determine the largest volume that a PME only needs to communicate */
  comm_vol_pme = 0;
  if ((dd->comm->npmenodes > 0) && (nc[XX] % dd->comm->npmenodes != 0)) {
    if (nc[XX] > dd->comm->npmenodes) {
      comm_vol_pme = (dd->comm->npmenodes==2 ? 1.0/3.0 : 0.5);
    } else {
      comm_vol_pme =
	1.0 - lcd(nc[XX],dd->comm->npmenodes)/(double)dd->comm->npmenodes;
    }
    /* Normalize the number of PME only nodes */
    comm_vol_pme /= dd->comm->npmenodes;
  }

  /* Add cost of pbc_dx for bondeds */
  cost_pbcdx = 0;
  if ((nc[XX] == 1 || nc[YY] == 1) || (nc[ZZ] == 1 && ir->ePBC != epbcXY)) {
    if ((dd->tric_dir[XX] && nc[XX] == 1) ||
	(dd->tric_dir[YY] && nc[YY] == 1))
      cost_pbcdx = pbcdxr*pbcdx_tric_fac/npp;
    else
      cost_pbcdx = pbcdxr*pbcdx_rect_fac/npp;
  }

  if (debug)
    fprintf(debug,
	    "nc %2d %2d %2d vol pp %6.4f pbcdx %6.4f pme %6.4f tot %6.4f\n",
	    nc[XX],nc[YY],nc[ZZ],
	    comm_vol,cost_pbcdx,comm_vol_pme,
	    comm_vol + cost_pbcdx + comm_vol_pme);

  return comm_vol + cost_pbcdx + comm_vol_pme;
}

static void assign_factors(gmx_domdec_t *dd,real limit,
			   matrix box,t_inputrec *ir,float pbcdxr,
			   int ndiv,int *div,int *mdiv,ivec try,ivec opt)
{
  int x,y,z,i;
  float ce;

  if (ndiv == 0) {
    ce = comm_cost_est(dd,limit,box,ir,pbcdxr,try);
    if (ce >= 0 && (opt[XX] == 0 ||
		    ce < comm_cost_est(dd,limit,box,ir,pbcdxr,opt)))
      copy_ivec(try,opt);

    return;
  }

  for(x=mdiv[0]; x>=0; x--) {
    for(i=0; i<x; i++)
      try[XX] *= div[0];
    for(y=mdiv[0]-x; y>=0; y--) {
      /* Check for requirement of no DD in z */
      if (ir->ePBC == epbcXY && ir->nwall < 2 && x + y != mdiv[0])
	continue;

      for(i=0; i<y; i++)
	try[YY] *= div[0];
      for(i=0; i<mdiv[0]-x-y; i++)
	try[ZZ] *= div[0];
      
      /* recurse */
      assign_factors(dd,limit,box,ir,pbcdxr,ndiv-1,div+1,mdiv+1,try,opt);
      
      for(i=0; i<mdiv[0]-x-y; i++)
	try[ZZ] /= div[0];
      for(i=0; i<y; i++)
	try[YY] /= div[0];
    }
    for(i=0; i<x; i++)
      try[XX] /= div[0];
  }
}

static bool bConstrLimit(t_topology *top,gmx_domdec_comm_t *comm)
{
  return (top->idef.il[F_CONSTR].nr > 0 && comm->bInterCGBondeds);
}

static void optimize_ncells(FILE *fplog,int nnodes_tot,int npme,real dlb_scale,
			    t_topology *top,matrix box,t_inputrec *ir,
			    gmx_domdec_t *dd,ivec nc)
{
  int npp,ndiv,*div,*mdiv,d;
  bool bExcl_pbcdx,bC;
  float pbcdxr;
  real limit;
  ivec try;
  gmx_domdec_comm_t *comm;
  char buf[STRLEN];

  comm = dd->comm;

  limit = comm->cutoff_mbody;

  comm->cutoff = max(max(ir->rlist,max(ir->rcoulomb,ir->rvdw)),
		     comm->cutoff_mbody);
  dd->nc[XX] = 1;
  dd->nc[YY] = 1;
  dd->nc[ZZ] = 1;
  set_tric_dir(dd,box);

  npp = nnodes_tot - npme;
  if (EEL_PME(ir->coulombtype)) {
    comm->npmenodes = (npme > 0 ? npme : npp);
  } else {
    comm->npmenodes = 0;
  }

  if (comm->bInterCGBondeds) {
    /* For Ewald exclusions pbc_dx is not called */
    bExcl_pbcdx =
      (EEL_EXCL_FORCES(ir->coulombtype) && !EEL_FULL(ir->coulombtype));
    pbcdxr = (double)n_bonded_dx(top,bExcl_pbcdx)/(double)top->atoms.nr;
    
    if (comm->bInterCGMultiBody && limit <= 0) {
      /* Here we should determine the minimum cell size from
       * the largest cg COG distance between atoms involved
       * in bonded interactions.
       */
      /* Set lower limit for the cell size to half the cut-off */
      limit = dd->comm->cutoff/2;
    }
    /* Take the maximum of the bonded and constraint distance limit */
    limit = max(limit,comm->cellsize_limit);
  } else {
    /* Every molecule is a single charge group: no pbc required */
    pbcdxr = 0;
  }
  /* Add a margin for DLB and/or pressure scaling */
  if (dd->bDynLoadBal) {
    if (dlb_scale >= 1.0)
      gmx_fatal(FARGS,"The value for option -dds should be smaller than 1");
    if (fplog)
      fprintf(fplog,"Scaling the initial minimum size with 1/%g (option -dds) = %g\n",dlb_scale,1/dlb_scale);
    limit /= dlb_scale;
  } else if (ir->epc != epcNO) {
    if (fplog)
      fprintf(fplog,"To account for pressure scaling, scaling the initial minimum size with %g\n",DD_GRID_MARGIN_PRES_SCALE);
    limit *= DD_GRID_MARGIN_PRES_SCALE;
  }

  if (fplog) {
    fprintf(fplog,"Optimizing the DD grid for %d cells with a minimum initial size of %.3f nm\n",npp,limit);
    if (limit > 0) {
      fprintf(fplog,"The maximum allowed number of cells is:");
      for(d=0; d<DIM; d++)
	fprintf(fplog," %c %d",
		dim2char(d),
		(d == ZZ && ir->ePBC == epbcXY && ir->nwall < 2) ? 1 :
		(int)(box[d][d]*dd->skew_fac[d]/limit));
      fprintf(fplog,"\n");
    }
  }

  if (debug)
    fprintf(debug,"Average nr of pbc_dx calls per atom %.2f\n",pbcdxr);

  /* Decompose npp in factors */
  ndiv = factorize(npp,&div,&mdiv);

  try[XX] = 1;
  try[YY] = 1;
  try[ZZ] = 1;
  clear_ivec(nc);
  assign_factors(dd,limit,box,ir,pbcdxr,ndiv,div,mdiv,try,nc);

  sfree(div);
  sfree(mdiv);

  if (nc[XX] == 0) {
    bC = (bConstrLimit(top,comm) && comm->cutoff_mbody < comm->cellsize_limit);
    sprintf(buf,"Change the number of nodes or mdrun option %s%s%s",
	    !bC ? "-rdd" : "-rcon",
	    dd->bDynLoadBal ? " or -dds" : "",
	    bC ? " or your LINCS settings" : "");
    gmx_fatal(FARGS,"There is no domain decomposition for %d nodes that is compatible with the given box and a minimum cell size of %g nm\n"
	      "%s\n"
	      "Look in the log file for details on the domain decomposition",
	      npp,limit,buf);
  }
}

static int multi_body_bondeds_count(t_ilist *il)
{
  int  ftype,n;

  n = 0;
  for(ftype=0; ftype<F_NRE; ftype++) {
    if ((interaction_function[ftype].flags & IF_BOND) && NRAL(ftype) > 2)
      n += il[ftype].nr/(1 + NRAL(ftype));
  }

  return n;
}

static int dd_nst_env(FILE *fplog,char *env_var,int def)
{
  char *val;
  int  nst;

  nst = def;
  val = getenv(env_var);
  if (val) {
    if (sscanf(val,"%d",&nst) <= 0) {
      nst = 1;
    }
    if (fplog)
      fprintf(fplog,"Found env.var. %s = %s, using value %d\n",
	      env_var,val,nst);
  }
  
  return nst;
}

gmx_domdec_t *init_domain_decomposition(FILE *fplog,t_commrec *cr,ivec nc,
					real comm_distance_min,real rconstr,
					bool bDynLoadBal,real dlb_scale,
					char *sizex,char *sizey,char *sizez,
					t_topology *top,matrix box,
					t_inputrec *ir)
{
  gmx_domdec_t *dd;
  gmx_domdec_comm_t *comm;
  int  d,i,j;
  char *warn="WARNING: Cycle counting is not supported on this architecture, will not use dynamic load balancing";

  if (fplog) {
    fprintf(fplog,
	    "\nInitializing Domain Decomposition on %d nodes\n",cr->nnodes);
  }

  snew(dd,1);
  snew(dd->comm,1);
  comm = dd->comm;
  snew(comm->cggl_flag,DIM*2);
  snew(comm->cgcm_state,DIM*2);

  comm->bSendRecv2 = dd_nst_env(fplog,"GMX_DD_SENDRECV2",0);
  comm->eFlop      = dd_nst_env(fplog,"GMX_DLB_FLOP",0);
  comm->nstSortCG  = dd_nst_env(fplog,"GMX_DD_SORT",1);
  nstDDDump        = dd_nst_env(fplog,"GMX_DD_DUMP",0);
  nstDDDumpGrid    = dd_nst_env(fplog,"GMX_DD_DUMP_GRID",0);
  if (comm->bSendRecv2 && fplog)
    fprintf(fplog,"Will use two sequential MPI_Sendrecv calls instead of two simultaneous non-blocking MPI_Irecv and MPI_Isend pairs for constraint and vsite communication\n");
  if (comm->eFlop) {
    if (fplog)
      fprintf(fplog,"Will load balance based on FLOP count\n");
    if (comm->eFlop > 1)
      srand(1+cr->nodeid);
    comm->bRecordLoad = TRUE;
  } else {
    comm->bRecordLoad = wallcycle_have_counter();
  }

  dd->bDynLoadBal = FALSE;
  if (bDynLoadBal) {
    if (comm->bRecordLoad) {
      dd->bDynLoadBal = TRUE;
    } else {
      if (fplog)
	fprintf(fplog,"\n%s\n\n",warn);
      fprintf(stderr,"\n%s\n\n",warn);
    }
  }
  if (fplog) {
    fprintf(fplog,"Will%s use dynamic load balancing\n",
	    dd->bDynLoadBal ? "" : " not");
  }
  dd->bGridJump = dd->bDynLoadBal;

  if (comm->nstSortCG) {
    if (fplog) {
      if (comm->nstSortCG == 1)
	fprintf(fplog,"Will sort the charge groups at every domain (re)decomposition\n");
      else
	fprintf(fplog,"Will sort the charge groups every %d steps\n",
		comm->nstSortCG);
    }
    snew(comm->sort,1);
  } else {
    if (fplog)
      fprintf(fplog,"Will not sort the charge groups\n");
  }

  comm->bInterCGBondeds = (top->cgs.nr > top->mols.nr);
  if (comm->bInterCGBondeds) {
    comm->bInterCGMultiBody = (multi_body_bondeds_count(top->idef.il) > 0);
  } else {
    comm->bInterCGMultiBody = FALSE;
  }

  comm->cutoff_mbody = comm_distance_min;
  comm->cellsize_limit = comm->cutoff_mbody;
  if (bConstrLimit(top,comm)) {
    /* There is a cell size limit due to the constraints (LINCS) */
    if (rconstr <= 0) {
      rconstr = constr_r_max(fplog,top,ir);
      if (fplog) {
	fprintf(fplog,
		"Estimated maximum distance required for LINCS: %.3f nm\n",
		rconstr);
	if (rconstr > comm->cutoff_mbody)
	  fprintf(fplog,"This distance will limit the DD cell size, you can override this with -rcon\n");
      }
    } else {
      if (fplog)
	fprintf(fplog,
		"User supplied maximum distance required for LINCS: %.3f nm\n",
		rconstr);
    }
    comm->cellsize_limit = max(comm->cellsize_limit,rconstr);
  }

  if (nc[XX] > 0) {
    copy_ivec(nc,dd->nc);
    if (cr->npmenodes == -1)
      cr->npmenodes = 0;
  } else {
    if (MASTER(cr)) {
      /* If the number of PME only node was not specified on the command line,
       * we assign PME only nodes with 12 or more nodes,
       * or when the PME grid does not match the number of nodes.
       */
      if (EEL_PME(ir->coulombtype) && cr->npmenodes < 0 && 
	  (cr->nnodes >= 12 ||
	   ir->nkx % cr->nnodes || ir->nky % cr->nnodes)) {
	cr->npmenodes = guess_npme(fplog,top,ir,box,cr->nnodes);
      } else if (cr->npmenodes < 0) {
	cr->npmenodes = 0;
      }
      
      optimize_ncells(fplog,cr->nnodes,cr->npmenodes,dlb_scale,
		      top,box,ir,dd,dd->nc);
    }
    gmx_bcast(sizeof(cr->npmenodes),&cr->npmenodes,cr);
    gmx_bcast(sizeof(dd->nc),dd->nc,cr);
  }

  if (fplog) {
    fprintf(fplog,
	    "Domain decomposition grid %d x %d x %d, separate PME nodes %d\n",
	    dd->nc[XX],dd->nc[YY],dd->nc[ZZ],cr->npmenodes);
  }

  dd->nnodes = dd->nc[XX]*dd->nc[YY]*dd->nc[ZZ];
  if (cr->nnodes - dd->nnodes != cr->npmenodes)
    gmx_fatal(FARGS,"The size of the domain decomposition grid (%d) does not match the number of nodes (%d). The total number of nodes is %d",
	      dd->nnodes,cr->nnodes - cr->npmenodes,cr->nnodes);
  if (cr->npmenodes > dd->nnodes)
    gmx_fatal(FARGS,"The number of separate PME node (%d) is larger than the number of PP nodes (%d), this is not supported.",cr->npmenodes,dd->nnodes);
  if (cr->npmenodes > 0) {
    comm->npmenodes = cr->npmenodes;
  } else {
    comm->npmenodes = dd->nnodes;
  }
  
  dd->ndim = 0;
  for(d=0; d<DIM; d++)
    if (dd->nc[d] > 1)
      dd->ndim++;

  snew(comm->slb_frac,DIM);
  if (!dd->bDynLoadBal) {
    comm->slb_frac[XX] = get_slb_frac(fplog,"x",dd->nc[XX],sizex);
    comm->slb_frac[YY] = get_slb_frac(fplog,"y",dd->nc[YY],sizey);
    comm->slb_frac[ZZ] = get_slb_frac(fplog,"z",dd->nc[ZZ],sizez);
  }
  
  if (comm->bRecordLoad) {
    if (dd->ndim > 1) {
      snew(comm->cell_d1,2);
      for(i=0; i<2; i++)
	snew(comm->cell_d1[i],2);
    }
    if (dd->ndim > 2) {
      snew(comm->cell_d2,2);
      for(i=0; i<2; i++) {
	snew(comm->cell_d2[i],2);
	for(j=0; j<2; j++)
	  snew(comm->cell_d2[i][j],2);
      }
    }
  }

  return dd;
}

void set_dd_parameters(FILE *fplog,gmx_domdec_t *dd,real dlb_scale,
		       t_topology *top,t_inputrec *ir,t_forcerec *fr,
		       matrix box)
{
  gmx_domdec_comm_t *comm;
  int  d,dim,npulse,npulse_d_max,npulse_d;
  ivec np;
  real limit;
  char buf[64];

  if (EEL_PME(ir->coulombtype)) {
    set_pme_x_limits(dd);
    set_slb_pme_dim_f(dd);
  } else {
    dd->comm->npmenodes = 0;
    if (dd->pme_nodeid >= 0)
      gmx_fatal(FARGS,
		"Can not have separate PME nodes without PME electrostatics");
  }

  if (ir->ePBC == epbcNONE)
    gmx_fatal(FARGS,"pbc type %s is not supported with domain decomposition",
	      epbc_names[epbcNONE]);
  if (ir->ePBC!=epbcXYZ && ir->nwall<2 && dd->nc[ZZ]>1)
    gmx_fatal(FARGS,"Can not do domain decomposition in the z-direction with pbc=%s,",epbc_names[ir->ePBC]);

  comm = dd->comm;

  /* If each molecule is a single charge group
   * or we use domain decomposition for each periodic dimension,
   * we do not need to take pbc into account for the bonded interactions.
   */
  if (!comm->bInterCGBondeds ||
      (dd->nc[XX]>1 && dd->nc[YY]>1 && (dd->nc[ZZ]>1 || fr->ePBC==epbcXY)))
    fr->bMolPBC = FALSE;
  else
    fr->bMolPBC = TRUE;

  if (ir->ns_type == ensSIMPLE)
    gmx_fatal(FARGS,"ns type %s is not supported with domain decomposition",
	      ens_names[ensSIMPLE]);
  
  dd->ndim = 0;
  if (getenv("GMX_DD_ORDER_ZYX")) {
    /* Decomposition order z,y,x */
    if (fplog)
      fprintf(fplog,"Using domain decomposition order z, y, x\n");
    for(dim=DIM-1; dim>=0; dim--) {
      if (dd->nc[dim] > 1)
	dd->dim[dd->ndim++] = dim;
    }
  } else {
    /* Decomposition order x,y,z */
    for(dim=0; dim<DIM; dim++) {
      if (dd->nc[dim] > 1)
	dd->dim[dd->ndim++] = dim;
    }
  }

  comm->cutoff = max(fr->rlistlong,comm->cutoff_mbody);
  if (debug)
    fprintf(debug,"The DD cut-off is %f\n",comm->cutoff);
  if (dd->bDynLoadBal) {
    /* Determine the maximum number of communication pulses in one dimension */

    comm->cellsize_limit = max(comm->cellsize_limit,comm->cutoff_mbody);

    /* Determine the maximum required number of grid pulses */
    if (comm->cellsize_limit >= comm->cutoff) {
      /* Only a single pulse is required */
      npulse = 1;
    } else if (comm->cellsize_limit > 0) {
      /* We round down slightly here to avoid overhead due to the latency
       * of extra communication calls when the cut-off would be only slightly
       * longer than the cell size. Later cellsize_limit is redetermined,
       * so we can not miss interactions due to this rounding.
       */
      npulse = (int)(0.96 + comm->cutoff/comm->cellsize_limit);
    } else {
      /* There is no cell size limit */
      npulse = dd->ncell;
    }

    if (npulse > 1) {
      /* See if we can do with less pulses, based on dlb_scale */
      set_tric_dir(dd,box);
      npulse_d_max = 0;
      for(d=0; d<dd->ndim; d++) {
	dim = dd->dim[d];
	npulse_d     = (int)(1 + dd->nc[dim]*comm->cutoff
			     /(box[dim][dim]*dd->skew_fac[dim]*dlb_scale));
	npulse_d_max = max(npulse_d_max,npulse_d);
      }
      npulse = min(npulse,npulse_d_max);
    }

    /* This env var can override npulse */
    d = dd_nst_env(fplog,"GMX_DD_NPULSE",0);
    if (d > 0)
      npulse = d;

    comm->maxpulse = 1;
    for(d=0; d<dd->ndim; d++) {
      comm->cd[d].np = min(npulse,dd->nc[dd->dim[d]]-1);
      snew(comm->cd[d].ind,comm->cd[d].np);
      comm->maxpulse = max(comm->maxpulse,comm->cd[d].np);
    }

    /* cellsize_limit is set for LINCS in init_domain_decomposition */
    comm->cellsize_limit = max(comm->cellsize_limit,
			       comm->cutoff/comm->maxpulse);
    comm->cellsize_limit = max(comm->cellsize_limit,comm->cutoff_mbody);
    /* Set the minimum cell size for each DD dimension */
    for(d=0; d<dd->ndim; d++) {
      if (comm->cd[d].np*comm->cellsize_limit >= comm->cutoff) {
	comm->cellsize_min[dd->dim[d]] = comm->cellsize_limit;
      } else {
	comm->cellsize_min[dd->dim[d]] = comm->cutoff/comm->cd[d].np;
      }
    }
    if (comm->cutoff_mbody <= 0) {
      comm->cutoff_mbody = comm->cellsize_limit;
    }
    if (DDMASTER(dd) && fplog) {
      fprintf(fplog,"The maximum number of communication pulses is:");
      for(d=0; d<dd->ndim; d++)
	fprintf(fplog," %c %d",dim2char(dd->dim[d]),comm->cd[d].np);
      fprintf(fplog,"\n");
      fprintf(fplog,"The minimum size for domain decomposition cells is %.3f nm\n",comm->cellsize_limit);
      fprintf(fplog,"The requested allowed shrink of DD cells (option -dds) is: %.2f\n",dlb_scale);
      fprintf(fplog,"The allowed shrink of domain decomposition cells is:");
      for(d=0; d<DIM; d++) {
	if (dd->nc[d] > 1)
	  fprintf(fplog," %c %.2f",
		  dim2char(d),
		  comm->cellsize_min[d]/(box[d][d]*dd->skew_fac[d]/dd->nc[d]));
      }
      fprintf(fplog,"\n\n");
    }
  } else {
    if (DDMASTER(dd) && fplog) {
      set_tric_dir(dd,box);
      set_dd_cell_sizes_slb(dd,box,FALSE,np);
      fprintf(fplog,"The initial number of communication pulses is:");
      for(d=0; d<dd->ndim; d++)
	fprintf(fplog," %c %d",dim2char(dd->dim[d]),np[dd->dim[d]]);
      fprintf(fplog,"\n");
      fprintf(fplog,"The initial domain decomposition cell size is:");
      for(d=0; d<DIM; d++) {
	if (dd->nc[d] > 1)
	  fprintf(fplog," %c %.2f nm",dim2char(d),dd->comm->cellsize_min[d]);
      }
      fprintf(fplog,"\n\n");
    }
  }

  if ((comm->bInterCGBondeds || dd->vsite_comm || dd->constraint_comm) &&
      DDMASTER(dd) && fplog) {
    fprintf(fplog,"The maximum allowed distance for atoms involved in interactions is:\n");
    if (dd->bDynLoadBal) {
      limit = dd->comm->cellsize_limit;
    } else {
      if (DYNAMIC_BOX(*ir))
	fprintf(fplog,"(these are initial values, they could change due to box deformation)\n");
      limit = dd->comm->cellsize_min[XX];
      for(d=1; d<DIM; d++)
	limit = min(limit,dd->comm->cellsize_min[d]);
    }
    if (comm->bInterCGBondeds)
      fprintf(fplog,"%40s  %-7s %6.3f nm\n",
	      "two-body bonded interactions","(-rdd)",comm->cutoff);
    if (comm->bInterCGBondeds)
      fprintf(fplog,"%40s  %-7s %6.3f nm\n",
	      "multi-body bonded interactions","(-rdd)",
	      dd->bGridJump ? comm->cutoff_mbody : min(comm->cutoff,limit));
    if (dd->vsite_comm)
      fprintf(fplog,"%40s  %-7s %6.3f nm\n",
	      "virtual site constructions","(-rcon)",limit);
    if (dd->constraint_comm) {
      sprintf(buf,"atoms separated by up to %d constraints",1+ir->nProjOrder);
      fprintf(fplog,"%40s  %-7s %6.3f nm\n",
	      buf,"(-rcon)",limit);
    }
    fprintf(fplog,"\n");
  }
}

static void merge_cg_buffers(int ncell,
			     gmx_domdec_comm_dim_t *cd, int pulse,
			     int  *ncg_cell,
			     int  *index_gl, int  *recv_i,
			     rvec *cg_cm,    rvec *recv_vr,
			     int *gcgs_index, int *cgindex)
{
  gmx_domdec_ind_t *ind,*ind_p;
  int p,cell,c,cg,cg0,cg1,cg_gl,nat;
  int shift,shift_at;

  ind = &cd->ind[pulse];
  
  /* First correct the already stored data */
  shift = ind->nrecv[ncell];
  for(cell=ncell-1; cell>=0; cell--) {
    shift -= ind->nrecv[cell];
    if (shift > 0) {
      /* Move the cg's present from previous grid pulses */
      cg0 = ncg_cell[ncell+cell];
      cg1 = ncg_cell[ncell+cell+1];
      cgindex[cg1+shift] = cgindex[cg1];
      for(cg=cg1-1; cg>=cg0; cg--) {
	index_gl[cg+shift] = index_gl[cg];
	copy_rvec(cg_cm[cg],cg_cm[cg+shift]);
	cgindex[cg+shift] = cgindex[cg];
      }
      /* Correct the already stored send indices for the shift */
      for(p=1; p<=pulse; p++) {
	ind_p = &cd->ind[p];
	cg0 = 0;
	for(c=0; c<cell; c++)
	  cg0 += ind_p->nsend[c];
	cg1 = cg0 + ind_p->nsend[cell];
	for(cg=cg0; cg<cg1; cg++) {
	  ind_p->index[cg] += shift;
	}
      }
    }
  }
  /* Merge in the communicated buffers */
  shift = 0;
  shift_at = 0;
  cg0 = 0;
  for(cell=0; cell<ncell; cell++) {
    cg1 = ncg_cell[ncell+cell+1] + shift;
    if (shift_at > 0) {
      /* Correct the old cg indices */
      for(cg=ncg_cell[ncell+cell]; cg<cg1; cg++) {
	cgindex[cg+1] += shift_at;
      }
    }
    for(cg=0; cg<ind->nrecv[cell]; cg++) {
      /* Copy this charge group from the buffer */
      index_gl[cg1] = recv_i[cg0];
      copy_rvec(recv_vr[cg0],cg_cm[cg1]);
      /* Add it to the cgindex */
      cg_gl = index_gl[cg1];
      nat = gcgs_index[cg_gl+1] - gcgs_index[cg_gl];
      cgindex[cg1+1] = cgindex[cg1] + nat;
      cg0++;
      cg1++;
      shift_at += nat;
    }
    shift += ind->nrecv[cell];
    ncg_cell[ncell+cell+1] = cg1;
  }
}

static void make_cell2at_index(gmx_domdec_comm_dim_t *cd,
			       int ncell,int cg0,const int *cgindex)
{
  int cg,cell,p;

  /* Store the atom block boundaries for easy copying of communication buffers
   */
  cg = cg0;
  for(cell=0; cell<ncell; cell++) {
    for(p=0; p<cd->np; p++) {
      cd->ind[p].cell2at0[cell] = cgindex[cg];
      cg += cd->ind[p].nrecv[cell];
      cd->ind[p].cell2at1[cell] = cgindex[cg];
    }
  }
}

static void setup_dd_communication(FILE *fplog,int step,
				   gmx_domdec_t *dd,int *gcgs_index,
				   matrix box,t_forcerec *fr)
{
  int dim_ind,dim,dim0,dim1=-1,p,nat_tot,ncell,cell,celli,cg0,cg1;
  int c,i,j,cg,cg_gl,nrcg;
  int *ncg_cell,pos_cg,*index_gl,*cgindex,*recv_i;
  gmx_domdec_comm_t *comm;
  gmx_domdec_comm_dim_t *cd;
  gmx_domdec_ind_t *ind;
  bool bTwoCut;
  real r_comm2,r_scomm2,r_bcomm2,r,r2,rb2,inv_ncg,tric_sh;
  real corner[DIM][4],corner_round_0=0,corner_round_1[4];
  real bcorner[DIM][4],bcorner_round_1[4];
  ivec tric_dist;
  rvec *cg_cm,*v_d,*v_0=NULL,*v_1=NULL,*recv_vr;
  real skew_fac2_d,skew_fac2_0=0,skew_fac2_1=0;
  int  nsend,nat;

  if (debug)
    fprintf(debug,"Setting up DD communication\n");

  comm  = dd->comm;
  cg_cm = fr->cg_cm;

  for(dim_ind=0; dim_ind<dd->ndim; dim_ind++) {
    dim = dd->dim[dim_ind];

    /* Check if we need to use triclinic distances */
    tric_dist[dim_ind] = 0;
    for(i=0; i<=dim_ind; i++)
      if (dd->tric_dir[dd->dim[i]])
	tric_dist[dim_ind] = 1;
  }

  bTwoCut = (dd->bGridJump && comm->bInterCGMultiBody && dd->ndim > 1 &&
	     dd->comm->cutoff_mbody < dd->comm->cutoff);

  dim0 = dd->dim[0];
  /* The first dimension is equal for all cells */
  corner[0][0] = dd->cell_x0[dim0];
  if (bTwoCut) {
    bcorner[0][0] = corner[0][0];
  }
  if (dd->ndim >= 2) {
    dim1 = dd->dim[1];
    /* This cell row is only seen from the first row */
    corner[1][0] = dd->cell_x0[dim1];
    /* All rows can see this row */
    corner[1][1] = dd->cell_x0[dim1];
    if (dd->bGridJump) {
      corner[1][1] = max(dd->cell_x0[dim1],comm->cell_d1[1][0]);
      if (comm->bInterCGMultiBody) {
	/* For the bonded distance we need the maximum */
	if (bTwoCut) {
	  bcorner[1][0] = corner[1][1];
	  bcorner[1][1] = corner[1][1];
	} else {
	  corner[1][0]  = corner[1][1];
	}
      }
    }
    /* Set the upper-right corner for rounding */
    corner_round_0 = dd->cell_x1[dim0];

    if (dd->ndim >= 3) {
      for(j=0; j<4; j++)
	corner[2][j] = dd->cell_x0[dd->dim[2]];
      if (dd->bGridJump) {
	/* Use the maximum of the i-cells that see a j-cell */
	for(i=0; i<dd->nicell; i++) {
	  for(j=dd->icell[i].j0; j<dd->icell[i].j1; j++) {
	    if (j >= 4) {
	      corner[2][j-4] =
		max(corner[2][j-4],
		    comm->cell_d2[dd->shift[i][dim0]][dd->shift[i][dim1]][0]);
	    }
	  }
	}
	if (comm->bInterCGMultiBody) {
	  /* For the bonded distance we need the maximum */
	  for(j=0; j<4; j++) {
	    if (bTwoCut)
	      bcorner[2][j] = corner[2][1];
	    else
	      corner[2][j]  = corner[2][1];
	  }
	}
      }
      
      /* Set the upper-right corner for rounding */
      /* Cell (0,0,0) and cell (1,0,0) can see cell 4 (0,1,1)
       * Only cell (0,0,0) can see cell 7 (1,1,1)
       */
      corner_round_1[0] = dd->cell_x1[dim1];
      corner_round_1[3] = dd->cell_x1[dim1];
      if (dd->bGridJump) {
	corner_round_1[0] = max(dd->cell_x1[dim1],comm->cell_d1[1][1]);
	if (comm->bInterCGMultiBody) {
	  /* For the bonded distance we need the maximum */
	  if (bTwoCut) {
	    bcorner_round_1[0] = corner_round_1[0];
	    bcorner_round_1[3] = corner_round_1[0];
	  } else {
	    corner_round_1[3]  = corner_round_1[0];
	  }
	}
      }
    }
  }

  r_comm2  = sqr(comm->cutoff);
  r_bcomm2 = sqr(comm->cutoff_mbody);

  /* Triclinic stuff */
  if (dd->ndim >= 2) {
    v_0 = comm->v[dim0];
    skew_fac2_0 = sqr(dd->skew_fac[dim0]);
  }
  if (dd->ndim >= 3) {
    v_1 = comm->v[dim1];
    skew_fac2_1 = sqr(dd->skew_fac[dim1]);
  }

  ncg_cell = dd->ncg_cell;
  index_gl = dd->index_gl;
  cgindex  = dd->cgindex;
  
  ncg_cell[0]        = 0;
  ncg_cell[1]        = dd->ncg_home;
  comm->cell_ncg1[0] = dd->ncg_home;
  pos_cg             = dd->ncg_home;

  nat_tot = dd->nat_home;
  ncell = 1;
  for(dim_ind=0; dim_ind<dd->ndim; dim_ind++) {
    dim = dd->dim[dim_ind];
    cd = &comm->cd[dim_ind];
    
    v_d = comm->v[dim];
    skew_fac2_d = sqr(dd->skew_fac[dim]);
    
    cd->bInPlace = TRUE;
    for(p=0; p<cd->np; p++) {
      ind = &cd->ind[p];
      nsend = 0;
      nat = 0;
      for(cell=0; cell<ncell; cell++) {
	celli = cell_perm[dim_ind][cell];
	if (p == 0) {
	  /* Here we permutate the cells to obtain a convenient order for ns */
	  cg0 = ncg_cell[celli];
	  cg1 = ncg_cell[celli+1];
	} else {
	  /* Look only at the cg's received in the previous grid pulse */
	  cg1 = ncg_cell[ncell+cell+1];
	  cg0 = cg1 - cd->ind[p-1].nrecv[cell];
	}
	ind->nsend[cell] = 0;
	for(cg=cg0; cg<cg1; cg++) {
	  r2  = 0;
	  rb2 = 0;
	  if (tric_dist[dim_ind] == 0) {
	    /* Rectangular direction, easy */
	    r = cg_cm[cg][dim] - corner[dim_ind][cell];
	    if (r > 0)
	      r2 += r*r;
	    if (bTwoCut) {
	      r = cg_cm[cg][dim] - bcorner[dim_ind][cell];
	      if (r > 0)
		rb2 += r*r;
	    }
	    /* Rounding gives at most a 16% reduction in communicated atoms */
	    if (dim_ind >= 1 && (celli == 1 || celli == 2)) {
	      r = cg_cm[cg][dim0] - corner_round_0;
	      /* This is the first dimension, so always r >= 0 */
	      r2 += r*r;
	      if (bTwoCut) {
		rb2 += r*r;
	      }
	    }
	    if (dim_ind == 2 && (celli == 2 || celli == 3)) {
	      r = cg_cm[cg][dim1] - corner_round_1[cell];
	      if (r > 0)
		r2 += r*r;
	      if (bTwoCut) {
		r = cg_cm[cg][dim1] - bcorner_round_1[cell];
		if (r > 0)
		  rb2 += r*r;
	      }
	    }
	  } else {
	    /* Triclinic direction, more complicated */
	    r = cg_cm[cg][dim] - corner[dim_ind][cell];
	    tric_sh = 0;
	    for(i=dim+1; i<DIM; i++)
	      tric_sh -= cg_cm[cg][i]*v_d[i][dim];
	    r += tric_sh;
	    if (r > 0)
	      r2 += r*r*skew_fac2_d;
	    if (bTwoCut) {
	      r = cg_cm[cg][dim] - bcorner[dim_ind][cell] + tric_sh;
	      if (r > 0)
		rb2 += r*r*skew_fac2_d;
	    }
	    /* Rounding, conservative as the skew_fac multiplication
	     * will slightly underestimate the distance.
	     */
	    if (dim_ind >= 1 && (celli == 1 || celli == 2)) {
	      r = cg_cm[cg][dim0] - corner_round_0;
	      for(i=dim0+1; i<DIM; i++)
		r -= cg_cm[cg][i]*v_0[i][dim0];
	      r2 += r*r*skew_fac2_0;
	      if (bTwoCut) {
		rb2 += r*r*skew_fac2_0;
	      }
	    }
	    if (dim_ind == 2 && (celli == 2 || celli == 3)) {
	      r = cg_cm[cg][dim1] - corner_round_1[cell];
	      tric_sh = 0;
	      for(i=dim1+1; i<DIM; i++)
		tric_sh -= cg_cm[cg][i]*v_1[i][dim1];
	      r += tric_sh;
	      if (r > 0)
		r2 += r*r*skew_fac2_1;
	      if (bTwoCut) {
		r = cg_cm[cg][dim1] - bcorner_round_1[cell] + tric_sh;
		if (r > 0)
		  rb2 += r*r*skew_fac2_1;
	      }
	    }
	  }
	  if (r2 < r_comm2 || (bTwoCut && rb2 < r_bcomm2)) {
	    /* Make an index to the local charge groups */
	    if (nsend+1 > ind->nalloc) {
	      ind->nalloc = over_alloc_large(nsend+1);
	      srenew(ind->index,ind->nalloc);
	    }
	    if (nsend+1 > comm->nalloc_int) {
	      comm->nalloc_int = over_alloc_large(nsend+1);
	      srenew(comm->buf_int,comm->nalloc_int);
	    }
	    ind->index[nsend] = cg;
	    comm->buf_int[nsend] = index_gl[cg];
	    ind->nsend[cell]++;
	    if (nsend+1 > comm->nalloc_vr) {
	      comm->nalloc_vr = over_alloc_large(nsend+1);
	      srenew(comm->buf_vr,comm->nalloc_vr);
	    }
	    if (dd->ci[dim] == 0) {
	      /* Correct cg_cm for pbc */
	      rvec_add(cg_cm[cg],box[dim],comm->buf_vr[nsend]);
	    } else {
	      copy_rvec(cg_cm[cg],comm->buf_vr[nsend]);
	    }
	    nsend++;
	    nat += cgindex[cg+1] - cgindex[cg];
	  }
	}
      }
      ind->nsend[ncell]   = nsend;
      ind->nsend[ncell+1] = nat;
      /* Communicate the number of cg's and atoms to receive */
      dd_sendrecv_int(dd, dim_ind, ddBackward,
		      ind->nsend, ncell+2,
		      ind->nrecv, ncell+2);
      if (p > 0) {
	/* We can receive in place if only the last cell is not empty */
	for(cell=0; cell<ncell-1; cell++)
	  if (ind->nrecv[cell] > 0)
	    cd->bInPlace = FALSE;
	if (!cd->bInPlace) {
	  /* The int buffer is only required here for the cg indices */
	  if (ind->nrecv[ncell] > comm->nalloc_int2) {
	    comm->nalloc_int2 = over_alloc_dd(ind->nrecv[ncell]);
	    srenew(comm->buf_int2,comm->nalloc_int2);
	  }
	  /* The rvec buffer is also required for atom buffers in dd_move_x */
	  i = max(cd->ind[0].nrecv[ncell+1],ind->nrecv[ncell+1]);
	  if (i > comm->nalloc_vr2) {
	    comm->nalloc_vr2 = over_alloc_dd(i);
	    srenew(comm->buf_vr2,comm->nalloc_vr2);
	  }
	}
      }

      /* Make space for the global cg indices */
      if (pos_cg + ind->nrecv[ncell] > dd->cg_nalloc
	  || dd->cg_nalloc == 0) {
	dd->cg_nalloc = over_alloc_dd(pos_cg + ind->nrecv[ncell]);
	srenew(index_gl,dd->cg_nalloc);
	srenew(cgindex,dd->cg_nalloc+1);
      }
      /* Communicate the global cg indices */
      if (cd->bInPlace) {
	recv_i = index_gl + pos_cg;
      } else {
	recv_i = comm->buf_int2;
      }
      dd_sendrecv_int(dd, dim_ind, ddBackward,
		      comm->buf_int, nsend,
		      recv_i,        ind->nrecv[ncell]);

      /* Make space for cg_cm */
      if (pos_cg + ind->nrecv[ncell] > fr->cg_nalloc) {
	dd_realloc_fr_cg(fr,pos_cg + ind->nrecv[ncell]);
	cg_cm = fr->cg_cm;
      }
      /* Communicate cg_cm */
      if (cd->bInPlace) {
	recv_vr = cg_cm + pos_cg;
      } else {
	recv_vr = comm->buf_vr2;
      }
      dd_sendrecv_rvec(dd, dim_ind, ddBackward,
		       comm->buf_vr, nsend,
		       recv_vr,      ind->nrecv[ncell]);

      /* Make the charge group index */
      if (cd->bInPlace) {
	cell = (p == 0 ? 0 : ncell - 1);
	while (cell < ncell) {
	  for(cg=0; cg<ind->nrecv[cell]; cg++) {
	    cg_gl = index_gl[pos_cg];
	    nrcg = gcgs_index[cg_gl+1] - gcgs_index[cg_gl];
	    cgindex[pos_cg+1] = cgindex[pos_cg] + nrcg;
	    pos_cg++;
	  }
	  if (p == 0)
	    comm->cell_ncg1[ncell+cell] = ind->nrecv[cell];
	  cell++;
	  ncg_cell[ncell+cell] = pos_cg;
	}
      } else {
	merge_cg_buffers(ncell,cd,p,ncg_cell,
			 index_gl,recv_i,cg_cm,recv_vr,
			 gcgs_index,cgindex);
	pos_cg += ind->nrecv[ncell];
      }
      nat_tot += ind->nrecv[ncell+1];
    }
    if (!cd->bInPlace) {
      /* Store the atom block for easy copying of communication buffers */
      make_cell2at_index(cd,ncell,ncg_cell[ncell],cgindex);
    }
    ncell += ncell;
  }
  dd->index_gl = index_gl;
  dd->cgindex  = cgindex;

  dd->ncg_tot = ncg_cell[dd->ncell];
  dd->nat_tot = nat_tot;
  comm->nat[ddnatHOME] = dd->nat_home;
  for(i=ddnatZONE; i<ddnatNR; i++)
    comm->nat[i] = dd->nat_tot;

  if (debug) {
    fprintf(debug,"Finished setting up DD communication, cells:");
    for(c=0; c<dd->ncell; c++)
      fprintf(debug," %d",dd->ncg_cell[c+1]-dd->ncg_cell[c]);
    fprintf(debug,"\n");
  }
}

static void set_cg_boundaries(gmx_domdec_t *dd)
{
  int c;

  for(c=0; c<dd->nicell; c++) {
    dd->icell[c].cg1  = dd->ncg_cell[c+1];
    dd->icell[c].jcg0 = dd->ncg_cell[dd->icell[c].j0];
    dd->icell[c].jcg1 = dd->ncg_cell[dd->icell[c].j1];
  }
}

static int comp_cgsort(const void *a,const void *b)
{
  int comp;

  gmx_cgsort_t *cga,*cgb;
  cga = (gmx_cgsort_t *)a;
  cgb = (gmx_cgsort_t *)b;

  comp = cga->nsc - cgb->nsc;
  if (comp == 0) {
    comp = cga->ind_gl - cgb->ind_gl;
  }
  
  return comp;
}

static void order_int_cg(int n,gmx_cgsort_t *sort,
			 int *a,int *buf)
{
  int i;
  
  /* Order the data */
  for(i=0; i<n; i++)
    buf[i] = a[sort[i].ind];

  /* Copy back to the original array */
  for(i=0; i<n; i++)
    a[i] = buf[i];
}

static void order_vec_cg(int n,gmx_cgsort_t *sort,
			 rvec *v,rvec *buf)
{
  int i;
  
  /* Order the data */
  for(i=0; i<n; i++)
    copy_rvec(v[sort[i].ind],buf[i]);

  /* Copy back to the original array */
  for(i=0; i<n; i++)
    copy_rvec(buf[i],v[i]);
}

static void order_vec_atom(int ncg,int *cgindex,gmx_cgsort_t *sort,
			   rvec *v,rvec *buf)
{
  int a,atot,cg,cg0,cg1,i;
  
  /* Order the data */
  a = 0;
  for(cg=0; cg<ncg; cg++) {
    cg0 = cgindex[sort[cg].ind];
    cg1 = cgindex[sort[cg].ind+1];
    for(i=cg0; i<cg1; i++) {
      copy_rvec(v[i],buf[a]);
      a++;
    }
  }
  atot = a;

  /* Copy back to the original array */
  for(a=0; a<atot; a++)
    copy_rvec(buf[a],v[a]);
}

static void ordered_sort(int nsort2,gmx_cgsort_t *sort2,
			 int nsort_new,gmx_cgsort_t *sort_new,
			 gmx_cgsort_t *sort1)
{
  int i1,i2,i_new;
  
  /* The new indices are not very ordered, so we qsort them */
  qsort(sort_new,nsort_new,sizeof(sort_new[0]),comp_cgsort);

  /* sort2 is already ordered, so now we can merge the two arrays */
  i1 = 0;
  i2 = 0;
  i_new = 0;
  while(i2 < nsort2 || i_new < nsort_new) {
    if (i2 == nsort2) {
      sort1[i1++] = sort_new[i_new++];
    } else if (i_new == nsort_new) {
      sort1[i1++] = sort2[i2++];
    } else if (sort2[i2].nsc < sort_new[i_new].nsc ||
	       (sort2[i2].nsc == sort_new[i_new].nsc &&
		sort2[i2].ind_gl < sort_new[i_new].ind_gl)) {
      sort1[i1++] = sort2[i2++];
    } else {
      sort1[i1++] = sort_new[i_new++];
    }
  }
}

static void dd_sort_state(gmx_domdec_t *dd,int ePBC,
			  rvec *cgcm,t_forcerec *fr,t_state *state,
			  int ncg_home_old)
{
  gmx_domdec_sort_t *sort;
  gmx_cgsort_t *cgsort,*sort_i;
  int  ncg_new,nsort2,nsort_new,i,cell_index,*ibuf,cgsize;
  rvec *vbuf;
  
  sort = dd->comm->sort;

  if (dd->ncg_home > sort->sort_nalloc) {
    sort->sort_nalloc = over_alloc_dd(dd->ncg_home);
    srenew(sort->sort1,sort->sort_nalloc);
    srenew(sort->sort2,sort->sort_nalloc);
  }

  if (ncg_home_old >= 0) {
    /* The charge groups that remained in the same ns grid cell
     * are completely ordered. So we can sort efficiently by sorting
     * the charge groups that did move into the stationary list.
     */
    ncg_new = 0;
    nsort2 = 0;
    nsort_new = 0;
    for(i=0; i<dd->ncg_home; i++) {
      /* Check if this cg did not move to another node */
      cell_index = fr->ns.grid->cell_index[i];
      if (cell_index !=  4*fr->ns.grid->ncells) {
	if (i >= ncg_home_old || cell_index != sort->sort1[i].nsc) {
	  /* This cg is new on this node or moved ns grid cell */
	  if (nsort_new >= sort->sort_new_nalloc) {
	    sort->sort_new_nalloc = over_alloc_dd(nsort_new+1);
	    srenew(sort->sort_new,sort->sort_new_nalloc);
	  }
	  sort_i = &(sort->sort_new[nsort_new++]);
	} else {
	  /* This cg did not move */
	  sort_i = &(sort->sort2[nsort2++]);
	}
	/* Sort on the ns grid cell indices and the global topology index */
	sort_i->nsc    = cell_index;
	sort_i->ind_gl = dd->index_gl[i];
	sort_i->ind    = i;
	ncg_new++;
      }
    }
    if (debug)
      fprintf(debug,"ordered sort cgs: stationary %d moved %d\n",
	      nsort2,nsort_new);
    /* Sort efficiently */
    ordered_sort(nsort2,sort->sort2,nsort_new,sort->sort_new,sort->sort1);
  } else {
    cgsort = sort->sort1;
    ncg_new = 0;
    for(i=0; i<dd->ncg_home; i++) {
      /* Sort on the ns grid cell indices and the global topology index */
      cgsort[i].nsc    = fr->ns.grid->cell_index[i];
      cgsort[i].ind_gl = dd->index_gl[i];
      cgsort[i].ind    = i;
      if (cgsort[i].nsc != 4*fr->ns.grid->ncells)
	ncg_new++;
    }
    if (debug)
      fprintf(debug,"qsort cgs: %d new home %d\n",dd->ncg_home,ncg_new);
    /* Determine the order of the charge groups using qsort */
    qsort(cgsort,dd->ncg_home,sizeof(cgsort[0]),comp_cgsort);
  }
  cgsort = sort->sort1;
  
  /* We alloc with the old size, since cgindex is still old */
  if (dd->cgindex[dd->ncg_home] > sort->vbuf_nalloc) {
    sort->vbuf_nalloc = over_alloc_dd(dd->cgindex[dd->ncg_home]);
    srenew(sort->vbuf,sort->vbuf_nalloc);
  }
  vbuf = sort->vbuf;

  /* Remove the charge groups which a no longer home here */
  dd->ncg_home = ncg_new;

  /* Reorder the state */
  order_vec_atom(dd->ncg_home,dd->cgindex,cgsort,state->x,vbuf);
  if (state->flags & STATE_HAS_V)
    order_vec_atom(dd->ncg_home,dd->cgindex,cgsort,state->v,vbuf);
  if (state->flags & STATE_HAS_SDX)
    order_vec_atom(dd->ncg_home,dd->cgindex,cgsort,state->sd_X,vbuf);
  if (state->flags & STATE_HAS_CGP)
    order_vec_atom(dd->ncg_home,dd->cgindex,cgsort,state->cg_p,vbuf);
  /* Reorder cgcm */
  order_vec_cg(dd->ncg_home,cgsort,cgcm,vbuf);

  if (dd->ncg_home+1 > sort->ibuf_nalloc) {
    sort->ibuf_nalloc = over_alloc_dd(dd->ncg_home+1);
    srenew(sort->ibuf,sort->ibuf_nalloc);
  }
  ibuf = sort->ibuf;
  /* Reorder the global cg index */
  order_int_cg(dd->ncg_home,cgsort,dd->index_gl,ibuf);
  /* Rebuild the local cg index */
  ibuf[0] = 0;
  for(i=0; i<dd->ncg_home; i++) {
    cgsize = dd->cgindex[cgsort[i].ind+1] - dd->cgindex[cgsort[i].ind];
    ibuf[i+1] = ibuf[i] + cgsize;
  }
  for(i=0; i<dd->ncg_home+1; i++)
    dd->cgindex[i] = ibuf[i];
  /* Set the home atom number */
  dd->nat_home = dd->cgindex[dd->ncg_home];

  /* Copy the sorted ns cell indices back to the ns grid struct */
  for(i=0; i<dd->ncg_home; i++)
    fr->ns.grid->cell_index[i] = cgsort[i].nsc;

  dd->bMasterHasAllCG = FALSE;
}

static void add_dd_statistics(gmx_domdec_t *dd)
{
  gmx_domdec_comm_t *comm;
  int ddnat;

  comm = dd->comm;

  for(ddnat=ddnatZONE; ddnat<ddnatNR; ddnat++)
    comm->sum_nat[ddnat-ddnatZONE] += (comm->nat[ddnat] - comm->nat[ddnat-1]);
  comm->ndecomp++;
}

void print_dd_statistics(t_commrec *cr,t_inputrec *ir,FILE *fplog)
{
  gmx_domdec_comm_t *comm;
  int ddnat;
  double av;
  
  comm = cr->dd->comm;

  gmx_sumd(ddnatNR-ddnatZONE,comm->sum_nat,cr);
  
  if (fplog == NULL)
    return;

  for(ddnat=ddnatZONE; ddnat<ddnatNR; ddnat++) {
    av = comm->sum_nat[ddnat-ddnatZONE]/comm->ndecomp;
    switch(ddnat) {
    case ddnatZONE:
      fprintf(fplog,
	      "DD av. #atoms communicated per step for force:  %d x %.1f\n",
	      2,av);
      break;
    case ddnatVSITE:
      if (cr->dd->vsite_comm) {
	fprintf(fplog,
		"DD av. #atoms communicated per step for vsites: %d x %.1f\n",
		(EEL_PME(ir->coulombtype) || ir->coulombtype==eelEWALD) ? 3 : 2,
		av);
      }
      break;
    case ddnatCON:
      if (cr->dd->constraint_comm) {
	fprintf(fplog,
		"DD av. #atoms communicated per step for LINCS:  %d x %.1f\n",
		1 + ir->nLincsIter,av);
      }
      break;
    default:
      gmx_incons("Unknown type for DD statistics");
    }
  }
  fprintf(fplog,"\n");

  if (comm->bRecordLoad)
    print_dd_load_av(fplog,cr->dd);
}

void dd_partition_system(FILE            *fplog,
			 int             step,
			 t_commrec       *cr,
			 bool            bMasterState,
			 t_state         *state_global,
			 t_topology      *top_global,
			 t_inputrec      *ir,
			 t_state         *state_local,
			 rvec            **f,
			 rvec            **buf,
			 t_mdatoms       *mdatoms,
			 t_topology      *top_local,
			 t_forcerec      *fr,
			 gmx_vsite_t     *vsite,
			 gmx_shellfc_t   shellfc,
			 gmx_constr_t    constr,
			 t_nrnb          *nrnb,
			 gmx_wallcycle_t wcycle,
			 bool            bVerbose)
{
  gmx_domdec_t *dd;
  int  i,j,n,cg0=0,ncg_home_old=-1;
  bool bLogLoad,bRedist,bSortCG;
  ivec ncells_old,np;

  dd = cr->dd;
  
  /* Check if we have recorded loads on the nodes */
  if (dd->comm->bRecordLoad && dd_load_count(dd->comm)) {
    /* Print load every nstlog, first and last step to the log file */
    bLogLoad = ((ir->nstlog > 0 && step % ir->nstlog == 0) ||
		!dd->comm->bFirstPrinted ||
		(step + ir->nstlist > ir->init_step + ir->nsteps));
    if (dd->bDynLoadBal || bLogLoad || bVerbose) {
      get_load_distribution(dd,wcycle);
      if (DDMASTER(dd)) {
	if (bLogLoad)
	  dd_print_load(fplog,dd,step-1);
	if (bVerbose)
	  dd_print_load_verbose(dd);
      }
      dd->comm->bFirstPrinted = TRUE;
    }
  }

  bRedist = FALSE;
  if (bMasterState) {
    get_cg_distribution(fplog,step,dd,
			&top_global->cgs,
			state_global->box,state_global->x);
    
    dd_distribute_state(dd,&top_global->cgs,
			state_global,state_local,f,buf);
    
    dd_make_local_cgs(dd,&top_local->cgs);

    if (dd->ncg_home > fr->cg_nalloc)
      dd_realloc_fr_cg(fr,dd->ncg_home);
    calc_cgcm(fplog,0,dd->ncg_home,
	      &top_local->cgs,state_local->x,fr->cg_cm);
    
    inc_nrnb(nrnb,eNR_CGCM,dd->nat_home);

    cg0 = 0;
  } else if (state_local->ddp_count != dd->ddp_count) {
    if (state_local->ddp_count > dd->ddp_count)
      gmx_fatal(FARGS,"Internal inconsistency state_local->ddp_count (%d) > dd->ddp_count (%d)",state_local->ddp_count,dd->ddp_count);

    if (state_local->ddp_count_cg_gl != state_local->ddp_count)
      gmx_fatal(FARGS,"Internal inconsistency state_local->ddp_count_cg_gl (%d) != state_local->ddp_count (%d)",state_local->ddp_count_cg_gl,state_local->ddp_count);

    /* Clear the old state */
    clear_dd_indices(dd,0);

    /* Build the new indices */
    rebuild_cgindex(dd,top_global->cgs.index,state_local);
    make_dd_indices(dd,top_global->cgs.index,0,fr);

    /* Redetermine the cg COMs */
    calc_cgcm(fplog,0,dd->ncg_home,
	      &top_local->cgs,state_local->x,fr->cg_cm);
    
    inc_nrnb(nrnb,eNR_CGCM,dd->nat_home);

    bRedist = dd->bDynLoadBal;
  } else {
    /* We have the full state, only redistribute the cgs */
    bRedist = TRUE;
  }

  set_dd_cell_sizes(dd,state_local->box,DYNAMIC_BOX(*ir),
		    bMasterState,FALSE,step);
  if (nstDDDumpGrid > 0 && step % nstDDDumpGrid == 0)
    write_dd_grid_pdb("dd_grid",step,dd,state_local->box);

  set_dd_ns_cell_sizes(dd,state_local->box,step);
  
  if (dd->comm->nstSortCG > 0) {
    bSortCG = (bMasterState ||
	       (bRedist && (step % dd->comm->nstSortCG == 0)));
  } else {
    bSortCG = FALSE;
  }
  dd->comm->bFilled_nsgrid_home = bSortCG;
  if (dd->comm->bFilled_nsgrid_home) {
    /* Initialize the ns grid */
    copy_ivec(fr->ns.grid->n,ncells_old);
    grid_first(fplog,fr->ns.grid,dd,fr->ePBC,state_local->box,fr->rlistlong,
	       dd->ncg_home);
    if (!bMasterState &&
	fr->ns.grid->n[XX] == ncells_old[XX] &&
	fr->ns.grid->n[YY] == ncells_old[YY] &&
	fr->ns.grid->n[ZZ] == ncells_old[ZZ]) {
      /* We can use the old order and ns grid cell indices of the charge groups
       * to sort the charge groups efficiently.
       */
      ncg_home_old = dd->ncg_home;
    } else {
      /* The new charge group order will be (quite) unrelated to the old one */
      ncg_home_old = -1;
    }
  }

  if (bRedist) {
    cg0 = dd_redistribute_cg(fplog,step,dd,&top_global->cgs,
			     state_local,f,buf,fr,mdatoms,
			     !bSortCG,nrnb);
    set_grid_ncg(fr->ns.grid,dd->ncg_home);
  }

  if (bSortCG) {
    /* Sort the state on charge group position.
     * This enables exact restarts from this step.
     * It also improves performance by about 15% with larger numbers
     * of atoms per node.
     */

    /* Fill the ns grid with the home cell, so we can sort with the indices */
    dd->ncg_cell[0] = 0;
    for(i=1; i<dd->ncell+1; i++)
      dd->ncg_cell[i] = dd->ncg_home;
    fill_grid(fplog,dd,fr->ns.grid,state_local->box,0,dd->ncg_home,fr->cg_cm);

    if (debug)
      fprintf(debug,"Step %d, sorting the %d home charge groups\n",
	      step,dd->ncg_home);
    dd_sort_state(dd,ir->ePBC,fr->cg_cm,fr,state_local,ncg_home_old);
    /* Rebuild all the indices */
    cg0 = 0;
  }

  /* Setup up the communication and communicate the coordinates */
  setup_dd_communication(fplog,step,dd,top_global->cgs.index,
			 state_local->box,fr);
  
  /* Set the indices */
  make_dd_indices(dd,top_global->cgs.index,cg0,fr);

  /* Set the charge group boundaries for neighbor searching */
  set_cg_boundaries(dd);

  /* Update the rest of the forcerec */
  fr->cg0 = 0;
  fr->hcg = dd->ncg_tot;
  if (fr->bTwinRange) {
    fr->f_twin_n = dd->nat_tot;
    if (fr->f_twin_n > fr->f_twin_nalloc) {
      fr->f_twin_nalloc = over_alloc_dd(fr->f_twin_n);
      srenew(fr->f_twin,fr->f_twin_nalloc);
    }
  }

  /*
  write_dd_pdb("dd_home",step,"dump",&top_global->atoms,cr,dd->nat_home,
	       state_local->x,state_local->box);
  */

  /* Extract a local topology from the global topology */
  for(i=0; i<dd->ndim; i++)
    np[dd->dim[i]] = dd->comm->cd[i].np;
  dd_make_local_top(fplog,dd,state_local->box,
		    dd->comm->cutoff,dd->comm->cellsize_min,np,
		    fr,vsite,top_global,top_local);

  /* Set up the special atom communication */
  n = dd->comm->nat[ddnatZONE];
  for(i=ddnatZONE+1; i<ddnatNR; i++) {
    switch(i) {
    case ddnatVSITE:
      if (vsite && vsite->n_intercg_vsite)
	n = dd_make_local_vsites(dd,n,top_local->idef.il);
      break;
    case ddnatCON:
      if (top_global->idef.il[F_CONSTR].nr > 0)
	n = dd_make_local_constraints(dd,n,
				      top_global->idef.il[F_CONSTR].iatoms,
				      constr,ir->nProjOrder);
      break;
    default:
      gmx_incons("Unknown special atom type setup");
    }
    dd->comm->nat[i] = n;
  }

  /* Make space for the extra coordinates for virtual site
   * or constraint communication.
   */
  state_local->natoms = dd->comm->nat[ddnatNR-1];
  if (state_local->natoms > state_local->nalloc)
    dd_realloc_state(state_local,f,buf,state_local->natoms);
  if (EEL_FULL(fr->eeltype)) {
    if (vsite && vsite->n_intercg_vsite)
      fr->f_el_recip_n = dd->comm->nat[ddnatVSITE];
    else
      fr->f_el_recip_n = (dd->n_intercg_excl ? dd->nat_tot : dd->nat_home);
    if (fr->f_el_recip_n > fr->f_el_recip_nalloc) {
      fr->f_el_recip_nalloc = over_alloc_dd(fr->f_el_recip_n);
      srenew(fr->f_el_recip,fr->f_el_recip_nalloc);
    }
  }

  /* We make the all mdatoms up to nat_tot_con.
   * We could save some work by only setting invmass
   * between nat_tot and nat_tot_con.
   */
  /* This call also sets the new number of home particles to dd->nat_home */
  atoms2md(&top_global->atoms,ir,top_global->idef.il[F_ORIRES].nr,
	   dd->comm->nat[ddnatCON],dd->gatindex,0,dd->nat_home,mdatoms);

  if (shellfc) {
    /* Make the local shell stuff, currently no communication is done */
    make_local_shells(cr->dd,mdatoms,shellfc);
  }

  if (!(cr->duty & DUTY_PME))
    /* Send the charges to our PME only node */
    gmx_pme_send_q(cr,mdatoms->nChargePerturbed,
		   mdatoms->chargeA,mdatoms->chargeB,
		   dd->comm->pme_maxshift);

  if (dd->constraints || top_global->idef.il[F_SETTLE].nr>0)
    set_constraints(constr,top_local,ir,mdatoms,dd);

  if (ir->ePull != epullNO)
    /* Update the local pull groups */
    dd_make_local_pull_groups(dd,ir->pull,mdatoms);

  add_dd_statistics(dd);

  /* Make sure we only count the cycles for this DD partitioning */
  clear_dd_cycle_counts(dd);

  /* Because the order of the atoms might have changed since
   * the last vsite construction, we need to communicate the constructing
   * atom coordinates again (for spreading the forces this MD step).
   */
  dd_move_x_vsites(dd,state_local->box,state_local->x);

  if (nstDDDump > 0 && step % nstDDDump == 0) {
    dd_move_x(dd,state_local->box,state_local->x,*buf);
    write_dd_pdb("dd_dump",step,"dump",&top_global->atoms,cr,
		 dd->comm->nat[ddnatVSITE],
		 state_local->x,state_local->box);
  }

  dd->ddp_count++;
  state_local->ddp_count = dd->ddp_count;
}
