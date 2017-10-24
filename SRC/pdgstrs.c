/*! \file
Copyright (c) 2003, The Regents of the University of California, through
Lawrence Berkeley National Laboratory (subject to receipt of any required 
approvals from U.S. Dept. of Energy) 

All rights reserved. 

The source code is distributed under BSD license, see the file License.txt
at the top-level directory.
*/


/*! @file 
 * \brief Solves a system of distributed linear equations A*X = B with a
 * general N-by-N matrix A using the LU factors computed previously.
 *
 * <pre>
 * -- Distributed SuperLU routine (version 2.3) --
 * Lawrence Berkeley National Lab, Univ. of California Berkeley.
 * October 15, 2008
 * </pre>
 */
#include <math.h>
#include "superlu_ddefs.h"

/*
 * Sketch of the algorithm for L-solve:
 * =======================
 *
 * Self-scheduling loop:
 *
 *   while ( not finished ) { .. use message counter to control
 *
 *      reveive a message;
 * 	
 * 	if ( message is Xk ) {
 * 	    perform local block modifications into lsum[];
 *                 lsum[i] -= L_i,k * X[k]
 *          if all local updates done, Isend lsum[] to diagonal process;
 *
 *      } else if ( message is LSUM ) { .. this must be a diagonal process 
 *          accumulate LSUM;
 *          if ( all LSUM are received ) {
 *              perform triangular solve for Xi;
 *              Isend Xi down to the current process column;
 *              perform local block modifications into lsum[];
 *          }
 *      }
 *   }
 *
 * 
 * Auxiliary data structures: lsum[] / ilsum (pointer to lsum array)
 * =======================
 *
 * lsum[] array (local)
 *   + lsum has "nrhs" columns, row-wise is partitioned by supernodes
 *   + stored by row blocks, column wise storage within a row block
 *   + prepend a header recording the global block number.
 *
 *         lsum[]                        ilsum[nsupers + 1]
 *
 *         -----
 *         | | |  <- header of size 2     ---
 *         --------- <--------------------| |
 *         | | | | |			  ---
 * 	   | | | | |	      |-----------| |		
 *         | | | | | 	      |           ---
 *	   ---------          |   |-------| |
 *         | | |  <- header   |   |       ---
 *         --------- <--------|   |  |----| |
 *         | | | | |		  |  |    ---
 * 	   | | | | |              |  |
 *         | | | | |              |  |
 *	   ---------              |  |
 *         | | |  <- header       |  |
 *         --------- <------------|  |
 *         | | | | |                 |
 * 	   | | | | |                 |
 *         | | | | |                 |
 *	   --------- <---------------|
 */
  
/*#define ISEND_IRECV*/

/*
 * Function prototypes
 */
#ifdef _CRAY
fortran void STRSM(_fcd, _fcd, _fcd, _fcd, int*, int*, double*,
		   double*, int*, double*, int*);
_fcd ftcs1;
_fcd ftcs2;
_fcd ftcs3;
#endif

/*! \brief
 *
 * <pre>
 * Purpose
 * =======
 *   Re-distribute B on the diagonal processes of the 2D process mesh.
 * 
 * Note
 * ====
 *   This routine can only be called after the routine pxgstrs_init(),
 *   in which the structures of the send and receive buffers are set up.
 *
 * Arguments
 * =========
 * 
 * B      (input) double*
 *        The distributed right-hand side matrix of the possibly
 *        equilibrated system.
 *
 * m_loc  (input) int (local)
 *        The local row dimension of matrix B.
 *
 * nrhs   (input) int (global)
 *        Number of right-hand sides.
 *
 * ldb    (input) int (local)
 *        Leading dimension of matrix B.
 *
 * fst_row (input) int (global)
 *        The row number of B's first row in the global matrix.
 *
 * ilsum  (input) int* (global)
 *        Starting position of each supernode in a full array.
 *
 * x      (output) double*
 *        The solution vector. It is valid only on the diagonal processes.
 *
 * ScalePermstruct (input) ScalePermstruct_t*
 *        The data structure to store the scaling and permutation vectors
 *        describing the transformations performed to the original matrix A.
 *
 * grid   (input) gridinfo_t*
 *        The 2D process mesh.
 *
 * SOLVEstruct (input) SOLVEstruct_t*
 *        Contains the information for the communication during the
 *        solution phase.
 *
 * Return value
 * ============
 * </pre>
 */

int_t
pdReDistribute_B_to_X(double *B, int_t m_loc, int nrhs, int_t ldb,
                      int_t fst_row, int_t *ilsum, double *x,
		      ScalePermstruct_t *ScalePermstruct,
		      Glu_persist_t *Glu_persist,
		      gridinfo_t *grid, SOLVEstruct_t *SOLVEstruct)
{
    int  *SendCnt, *SendCnt_nrhs, *RecvCnt, *RecvCnt_nrhs;
    int  *sdispls, *sdispls_nrhs, *rdispls, *rdispls_nrhs;
    int  *ptr_to_ibuf, *ptr_to_dbuf;
    int_t  *perm_r, *perm_c; /* row and column permutation vectors */
    int_t  *send_ibuf, *recv_ibuf;
    double *send_dbuf, *recv_dbuf;
    int_t  *xsup, *supno;
    int_t  i, ii, irow, gbi, j, jj, k, knsupc, l, lk;
    int    p, procs;
    pxgstrs_comm_t *gstrs_comm = SOLVEstruct->gstrs_comm;

	MPI_Request req_i, req_d, *req_send, *req_recv;
	MPI_Status status, *status_send, *status_recv;
	int Nreq_recv, Nreq_send, pp;
		
#if ( DEBUGlevel>=1 )
    CHECK_MALLOC(grid->iam, "Enter pdReDistribute_B_to_X()");
#endif

    /* ------------------------------------------------------------
       INITIALIZATION.
       ------------------------------------------------------------*/
    perm_r = ScalePermstruct->perm_r;
    perm_c = ScalePermstruct->perm_c;
    procs = grid->nprow * grid->npcol;
    xsup = Glu_persist->xsup;
    supno = Glu_persist->supno;
    SendCnt      = gstrs_comm->B_to_X_SendCnt;
    SendCnt_nrhs = gstrs_comm->B_to_X_SendCnt +   procs;
    RecvCnt      = gstrs_comm->B_to_X_SendCnt + 2*procs;
    RecvCnt_nrhs = gstrs_comm->B_to_X_SendCnt + 3*procs;
    sdispls      = gstrs_comm->B_to_X_SendCnt + 4*procs;
    sdispls_nrhs = gstrs_comm->B_to_X_SendCnt + 5*procs;
    rdispls      = gstrs_comm->B_to_X_SendCnt + 6*procs;
    rdispls_nrhs = gstrs_comm->B_to_X_SendCnt + 7*procs;
    ptr_to_ibuf  = gstrs_comm->ptr_to_ibuf;
    ptr_to_dbuf  = gstrs_comm->ptr_to_dbuf;

    /* ------------------------------------------------------------
       NOW COMMUNICATE THE ACTUAL DATA.
       ------------------------------------------------------------*/
    k = sdispls[procs-1] + SendCnt[procs-1]; /* Total number of sends */
    l = rdispls[procs-1] + RecvCnt[procs-1]; /* Total number of receives */
    if ( !(send_ibuf = intMalloc_dist(k + l)) )
        ABORT("Malloc fails for send_ibuf[].");
    recv_ibuf = send_ibuf + k;
    if ( !(send_dbuf = doubleMalloc_dist((k + l)* (size_t)nrhs)) )
        ABORT("Malloc fails for send_dbuf[].");
    recv_dbuf = send_dbuf + k * nrhs;
    if ( !(req_send = (MPI_Request*) SUPERLU_MALLOC(procs*sizeof(MPI_Request))) )
	ABORT("Malloc fails for req_send[].");	
    if ( !(req_recv = (MPI_Request*) SUPERLU_MALLOC(procs*sizeof(MPI_Request))) )
	ABORT("Malloc fails for req_recv[].");
    if ( !(status_send = (MPI_Status*) SUPERLU_MALLOC(procs*sizeof(MPI_Status))) )
	ABORT("Malloc fails for status_send[].");
    if ( !(status_recv = (MPI_Status*) SUPERLU_MALLOC(procs*sizeof(MPI_Status))) )
	ABORT("Malloc fails for status_recv[].");

    for (p = 0; p < procs; ++p) {
        ptr_to_ibuf[p] = sdispls[p];
        ptr_to_dbuf[p] = sdispls[p] * nrhs;
    }

    /* Copy the row indices and values to the send buffer. */
    for (i = 0, l = fst_row; i < m_loc; ++i, ++l) {
        irow = perm_c[perm_r[l]]; /* Row number in Pc*Pr*B */
	gbi = BlockNum( irow );
	p = PNUM( PROW(gbi,grid), PCOL(gbi,grid), grid ); /* Diagonal process */
	k = ptr_to_ibuf[p];
	send_ibuf[k] = irow;
	k = ptr_to_dbuf[p];
	RHS_ITERATE(j) { /* RHS is stored in row major in the buffer. */
	    send_dbuf[k++] = B[i + j*ldb];
	}
	++ptr_to_ibuf[p];
	ptr_to_dbuf[p] += nrhs;
    }

	
#if 1	
    /* Communicate the (permuted) row indices. */
    MPI_Alltoallv(send_ibuf, SendCnt, sdispls, mpi_int_t,
		  recv_ibuf, RecvCnt, rdispls, mpi_int_t, grid->comm);

    /* Communicate the numerical values. */
    MPI_Alltoallv(send_dbuf, SendCnt_nrhs, sdispls_nrhs, MPI_DOUBLE,
		  recv_dbuf, RecvCnt_nrhs, rdispls_nrhs, MPI_DOUBLE,
		  grid->comm);
    
#else	
	
    /* Communicate the (permuted) row indices. */
    MPI_Ialltoallv(send_ibuf, SendCnt, sdispls, mpi_int_t,
		  recv_ibuf, RecvCnt, rdispls, mpi_int_t, grid->comm, &req_i);

    /* Communicate the numerical values. */
    MPI_Ialltoallv(send_dbuf, SendCnt_nrhs, sdispls_nrhs, MPI_DOUBLE,
		  recv_dbuf, RecvCnt_nrhs, rdispls_nrhs, MPI_DOUBLE,
		  grid->comm, &req_d);	
	MPI_Wait(&req_i,&status);
	MPI_Wait(&req_d,&status);
	
#endif	



// MPI_Barrier( grid->comm );


// Nreq_send=0;
// for (pp=0;pp<procs;pp++){
	// if(SendCnt[pp]>0){
		// MPI_Isend(&send_ibuf[sdispls[pp]], SendCnt[pp], mpi_int_t, pp, 0, grid->comm,
		// &req_send[Nreq_send] );
		// Nreq_send++;
	// }
// }

// Nreq_recv=0;
// for (pp=0;pp<procs;pp++){
	// if(RecvCnt[pp]>0){
		// MPI_Irecv(&recv_ibuf[rdispls[pp]], RecvCnt[pp], mpi_int_t, pp, 0, grid->comm,
		// &req_recv[Nreq_recv] );
		// Nreq_recv++;
	// }
// }

// if(Nreq_send>0)MPI_Waitall(Nreq_send,req_send,status_send);
// if(Nreq_recv>0)MPI_Waitall(Nreq_recv,req_recv,status_recv);


// Nreq_send=0;
// for (pp=0;pp<procs;pp++){
	// if(SendCnt_nrhs[pp]>0){
		// MPI_Isend(&send_dbuf[sdispls_nrhs[pp]], SendCnt_nrhs[pp], MPI_DOUBLE, pp, 1, grid->comm,
		// &req_send[Nreq_send] );
		// Nreq_send++;
	// }
// }
// Nreq_recv=0;
// for (pp=0;pp<procs;pp++){
	// if(RecvCnt_nrhs[pp]>0){
		// MPI_Irecv(&recv_dbuf[rdispls_nrhs[pp]], RecvCnt_nrhs[pp], MPI_DOUBLE, pp, 1, grid->comm,
		// &req_recv[Nreq_recv] );
		// Nreq_recv++;
	// }
// }

// if(Nreq_send>0)MPI_Waitall(Nreq_send,req_send,status_send);
// if(Nreq_recv>0)MPI_Waitall(Nreq_recv,req_recv,status_recv);



    /* ------------------------------------------------------------
       Copy buffer into X on the diagonal processes.
       ------------------------------------------------------------*/
    ii = 0;
    for (p = 0; p < procs; ++p) {
        jj = rdispls_nrhs[p];
        for (i = 0; i < RecvCnt[p]; ++i) {
	    /* Only the diagonal processes do this; the off-diagonal processes
	       have 0 RecvCnt. */
	    irow = recv_ibuf[ii]; /* The permuted row index. */
	    k = BlockNum( irow );
	    knsupc = SuperSize( k );
	    lk = LBi( k, grid );  /* Local block number. */
	    l = X_BLK( lk );
	    x[l - XK_H] = k;      /* Block number prepended in the header. */
	    irow = irow - FstBlockC(k); /* Relative row number in X-block */
	    RHS_ITERATE(j) {
	        x[l + irow + j*knsupc] = recv_dbuf[jj++];
	    }
	    ++ii;
	}
    }

    SUPERLU_FREE(send_ibuf);
    SUPERLU_FREE(send_dbuf);
    SUPERLU_FREE(req_send);
    SUPERLU_FREE(req_recv);
    SUPERLU_FREE(status_send);
    SUPERLU_FREE(status_recv);
    
#if ( DEBUGlevel>=1 )
    CHECK_MALLOC(grid->iam, "Exit pdReDistribute_B_to_X()");
#endif
    return 0;
} /* pdReDistribute_B_to_X */

/*! \brief
 *
 * <pre>
 * Purpose
 * =======
 *   Re-distribute X on the diagonal processes to B distributed on all
 *   the processes.
 *
 * Note
 * ====
 *   This routine can only be called after the routine pxgstrs_init(),
 *   in which the structures of the send and receive buffers are set up.
 * </pre>
 */

int_t
pdReDistribute_X_to_B(int_t n, double *B, int_t m_loc, int_t ldb, int_t fst_row,
		      int_t nrhs, double *x, int_t *ilsum,
		      ScalePermstruct_t *ScalePermstruct,
		      Glu_persist_t *Glu_persist, gridinfo_t *grid,
		      SOLVEstruct_t *SOLVEstruct)
{
    int_t  i, ii, irow, j, jj, k, knsupc, nsupers, l, lk;
    int_t  *xsup, *supno;
    int  *SendCnt, *SendCnt_nrhs, *RecvCnt, *RecvCnt_nrhs;
    int  *sdispls, *rdispls, *sdispls_nrhs, *rdispls_nrhs;
    int  *ptr_to_ibuf, *ptr_to_dbuf;
    int_t  *send_ibuf, *recv_ibuf;
    double *send_dbuf, *recv_dbuf;
    int_t  *row_to_proc = SOLVEstruct->row_to_proc; /* row-process mapping */
    pxgstrs_comm_t *gstrs_comm = SOLVEstruct->gstrs_comm;
    int  iam, p, q, pkk, procs;
    int_t  num_diag_procs, *diag_procs;
	MPI_Request req_i, req_d, *req_send, *req_recv;
	MPI_Status status, *status_send, *status_recv;
	int Nreq_recv, Nreq_send, pp;
	
#if ( DEBUGlevel>=1 )
    CHECK_MALLOC(grid->iam, "Enter pdReDistribute_X_to_B()");
#endif

    /* ------------------------------------------------------------
       INITIALIZATION.
       ------------------------------------------------------------*/
    xsup = Glu_persist->xsup;
    supno = Glu_persist->supno;
    nsupers = Glu_persist->supno[n-1] + 1;
    iam = grid->iam;
    procs = grid->nprow * grid->npcol;
 
    SendCnt      = gstrs_comm->X_to_B_SendCnt;
    SendCnt_nrhs = gstrs_comm->X_to_B_SendCnt +   procs;
    RecvCnt      = gstrs_comm->X_to_B_SendCnt + 2*procs;
    RecvCnt_nrhs = gstrs_comm->X_to_B_SendCnt + 3*procs;
    sdispls      = gstrs_comm->X_to_B_SendCnt + 4*procs;
    sdispls_nrhs = gstrs_comm->X_to_B_SendCnt + 5*procs;
    rdispls      = gstrs_comm->X_to_B_SendCnt + 6*procs;
    rdispls_nrhs = gstrs_comm->X_to_B_SendCnt + 7*procs;
    ptr_to_ibuf  = gstrs_comm->ptr_to_ibuf;
    ptr_to_dbuf  = gstrs_comm->ptr_to_dbuf;

    k = sdispls[procs-1] + SendCnt[procs-1]; /* Total number of sends */
    l = rdispls[procs-1] + RecvCnt[procs-1]; /* Total number of receives */
    if ( !(send_ibuf = intMalloc_dist(k + l)) )
        ABORT("Malloc fails for send_ibuf[].");
    recv_ibuf = send_ibuf + k;
    if ( !(send_dbuf = doubleMalloc_dist((k + l)*nrhs)) )
        ABORT("Malloc fails for send_dbuf[].");
    if ( !(req_send = (MPI_Request*) SUPERLU_MALLOC(procs*sizeof(MPI_Request))) )
	ABORT("Malloc fails for req_send[].");	
    if ( !(req_recv = (MPI_Request*) SUPERLU_MALLOC(procs*sizeof(MPI_Request))) )
	ABORT("Malloc fails for req_recv[].");
    if ( !(status_send = (MPI_Status*) SUPERLU_MALLOC(procs*sizeof(MPI_Status))) )
	ABORT("Malloc fails for status_send[].");
    if ( !(status_recv = (MPI_Status*) SUPERLU_MALLOC(procs*sizeof(MPI_Status))) )
	ABORT("Malloc fails for status_recv[].");	
	
							 
    recv_dbuf = send_dbuf + k * nrhs;
    for (p = 0; p < procs; ++p) {
        ptr_to_ibuf[p] = sdispls[p];
        ptr_to_dbuf[p] = sdispls_nrhs[p];
    }
    num_diag_procs = SOLVEstruct->num_diag_procs;
    diag_procs = SOLVEstruct->diag_procs;

    for (p = 0; p < num_diag_procs; ++p) {  /* For all diagonal processes. */
	pkk = diag_procs[p];
	if ( iam == pkk ) {
	    for (k = p; k < nsupers; k += num_diag_procs) {
		knsupc = SuperSize( k );
		lk = LBi( k, grid ); /* Local block number */
		irow = FstBlockC( k );
		l = X_BLK( lk );
		for (i = 0; i < knsupc; ++i) {
#if 0
		    ii = inv_perm_c[irow]; /* Apply X <== Pc'*Y */
#else
		    ii = irow;
#endif
		    q = row_to_proc[ii];
		    jj = ptr_to_ibuf[q];
		    send_ibuf[jj] = ii;
		    jj = ptr_to_dbuf[q];
		    RHS_ITERATE(j) { /* RHS stored in row major in buffer. */
		        send_dbuf[jj++] = x[l + i + j*knsupc];
		    }
		    ++ptr_to_ibuf[q];
		    ptr_to_dbuf[q] += nrhs;
		    ++irow;
		}
	    }
	}
    }
    
    /* ------------------------------------------------------------
        COMMUNICATE THE (PERMUTED) ROW INDICES AND NUMERICAL VALUES.
       ------------------------------------------------------------*/
   
#if 1
   
	MPI_Alltoallv(send_ibuf, SendCnt, sdispls, mpi_int_t,
		  recv_ibuf, RecvCnt, rdispls, mpi_int_t, grid->comm);
    MPI_Alltoallv(send_dbuf, SendCnt_nrhs, sdispls_nrhs, MPI_DOUBLE, 
		  recv_dbuf, RecvCnt_nrhs, rdispls_nrhs, MPI_DOUBLE,
		  grid->comm);

#else
    MPI_Ialltoallv(send_ibuf, SendCnt, sdispls, mpi_int_t,
		  recv_ibuf, RecvCnt, rdispls, mpi_int_t, grid->comm,&req_i);
    MPI_Ialltoallv(send_dbuf, SendCnt_nrhs, sdispls_nrhs, MPI_DOUBLE, 
		  recv_dbuf, RecvCnt_nrhs, rdispls_nrhs, MPI_DOUBLE,
		  grid->comm,&req_d);
		  
	MPI_Wait(&req_i,&status);
	MPI_Wait(&req_d,&status);		 
#endif	

// MPI_Barrier( grid->comm );
// Nreq_send=0;
// for (pp=0;pp<procs;pp++){
	// if(SendCnt[pp]>0){
		// MPI_Isend(&send_ibuf[sdispls[pp]], SendCnt[pp], mpi_int_t, pp, 0, grid->comm,
		// &req_send[Nreq_send] );
		// Nreq_send++;
	// }
// }

// Nreq_recv=0;
// for (pp=0;pp<procs;pp++){
	// if(RecvCnt[pp]>0){
		// MPI_Irecv(&recv_ibuf[rdispls[pp]], RecvCnt[pp], mpi_int_t, pp, 0, grid->comm,
		// &req_recv[Nreq_recv] );
		// Nreq_recv++;
	// }
// }

// if(Nreq_send>0)MPI_Waitall(Nreq_send,req_send,status_send);
// if(Nreq_recv>0)MPI_Waitall(Nreq_recv,req_recv,status_recv);
// // MPI_Barrier( grid->comm );

// Nreq_send=0;
// for (pp=0;pp<procs;pp++){
	// if(SendCnt_nrhs[pp]>0){
		// MPI_Isend(&send_dbuf[sdispls_nrhs[pp]], SendCnt_nrhs[pp], MPI_DOUBLE, pp, 1, grid->comm,
		// &req_send[Nreq_send] );
		// Nreq_send++;
	// }
// }
// Nreq_recv=0;
// for (pp=0;pp<procs;pp++){
	// if(RecvCnt_nrhs[pp]>0){
		// MPI_Irecv(&recv_dbuf[rdispls_nrhs[pp]], RecvCnt_nrhs[pp], MPI_DOUBLE, pp, 1, grid->comm,
		// &req_recv[Nreq_recv] );
		// Nreq_recv++;
	// }
// }

// if(Nreq_send>0)MPI_Waitall(Nreq_send,req_send,status_send);
// if(Nreq_recv>0)MPI_Waitall(Nreq_recv,req_recv,status_recv);
// // MPI_Barrier( grid->comm );





    /* ------------------------------------------------------------
       COPY THE BUFFER INTO B.
       ------------------------------------------------------------*/
    for (i = 0, k = 0; i < m_loc; ++i) {
	irow = recv_ibuf[i];
	irow -= fst_row; /* Relative row number */
	RHS_ITERATE(j) { /* RHS is stored in row major in the buffer. */
	    B[irow + j*ldb] = recv_dbuf[k++];
	}
    }

    SUPERLU_FREE(send_ibuf);
    SUPERLU_FREE(send_dbuf);
    SUPERLU_FREE(req_send);
    SUPERLU_FREE(req_recv);
    SUPERLU_FREE(status_send);
    SUPERLU_FREE(status_recv);
	
#if ( DEBUGlevel>=1 )
    CHECK_MALLOC(grid->iam, "Exit pdReDistribute_X_to_B()");
#endif
    return 0;

} /* pdReDistribute_X_to_B */



void
pdCompute_Diag_Inv(int_t n, LUstruct_t *LUstruct,gridinfo_t *grid, SuperLUStat_t *stat, int *info)
{
    Glu_persist_t *Glu_persist = LUstruct->Glu_persist;
    LocalLU_t *Llu = LUstruct->Llu;

    double *lusup;
    double *recvbuf, *tempv;
    double *Linv;/* Inverse of diagonal block */
    double *Uinv;/* Inverse of diagonal block */

    int_t  kcol, krow, mycol, myrow;
    int_t  i, ii, il, j, jj, k, lb, ljb, lk, lptr, luptr;
    int_t  nb, nlb,nlb_nodiag, nub, nsupers;
    int_t  *xsup, *supno, *lsub, *usub;
    int_t  *ilsum;    /* Starting position of each supernode in lsum (LOCAL)*/
    int    Pc, Pr, iam;
    int    knsupc, nsupr;
    int    ldalsum;   /* Number of lsum entries locally owned. */
    int    maxrecvsz, p, pi;
    int_t  **Lrowind_bc_ptr;
    double **Lnzval_bc_ptr;
    double **Linv_bc_ptr;
    double **Uinv_bc_ptr;
	int INFO;
	double t;
 
#if ( PROFlevel>=1 )
    t = SuperLU_timer_();
#endif 
 	
	// printf("wocao \n");
	// fflush(stdout);
	if(grid->iam==0){
	printf("computing inverse of diagonal blocks...\n");
	fflush(stdout);
	}
    /*
     * Initialization.
     */
    iam = grid->iam;
    Pc = grid->npcol;
    Pr = grid->nprow;
    myrow = MYROW( iam, grid );
    mycol = MYCOL( iam, grid );
    xsup = Glu_persist->xsup;
    supno = Glu_persist->supno;
    nsupers = supno[n-1] + 1;
    Lrowind_bc_ptr = Llu->Lrowind_bc_ptr;
    Linv_bc_ptr = Llu->Linv_bc_ptr;
    Uinv_bc_ptr = Llu->Uinv_bc_ptr;
    Lnzval_bc_ptr = Llu->Lnzval_bc_ptr;
    nlb = CEILING( nsupers, Pr ); /* Number of local block rows. */


	Llu->inv = 1;

    /*---------------------------------------------------
     * Compute inverse of L(lk,lk).
     *---------------------------------------------------*/

	for (k = 0; k < nsupers; ++k) {
	    krow = PROW( k, grid );
	    if ( myrow == krow ) {
		lk = LBi( k, grid );    /* local block number */
		kcol = PCOL( k, grid );
		if ( mycol == kcol ) { /* diagonal process */

		lk = LBj( k, grid ); /* Local block number, column-wise. */
		lsub = Lrowind_bc_ptr[lk];
		lusup = Lnzval_bc_ptr[lk];
		Linv = Linv_bc_ptr[lk];
		Uinv = Uinv_bc_ptr[lk];
		nsupr = lsub[1];	
		knsupc = SuperSize( k );
	 
		for (j=0 ; j<knsupc; j++){
			Linv[j*knsupc+j] = 1.0;

			for (i=j+1 ; i<knsupc; i++){
			Linv[j*knsupc+i] = lusup[j*nsupr+i];	
			}

			for (i=0 ; i<j+1; i++){
			Uinv[j*knsupc+i] = lusup[j*nsupr+i];	
			}			
			
		}
		
#ifdef _CRAY
		ABORT("Cray blas dtrtri not implemented\n");
#elif defined (USE_VENDOR_BLAS)
		dtrtri_("L","U",&knsupc,Linv,&knsupc,&INFO);		  	
	    dtrtri_("U","N",&knsupc,Uinv,&knsupc,&INFO);	
#else
		ABORT("internal blas dtrtri not implemented\n");
#endif			
		
		}
	    }
	}
	
#if ( PROFlevel>=1 )
if(grid->iam==0){
    t = SuperLU_timer_() - t;
    printf(".. L-diag_inv time\t%10.5f\n", t);
	fflush(stdout);
}
#endif	
	
    return;
}




/*! \brief
 *
 * <pre>
 * Purpose
 * =======
 *
 * PDGSTRS solves a system of distributed linear equations
 * A*X = B with a general N-by-N matrix A using the LU factorization
 * computed by PDGSTRF.
 * If the equilibration, and row and column permutations were performed,
 * the LU factorization was performed for A1 where
 *     A1 = Pc*Pr*diag(R)*A*diag(C)*Pc^T = L*U
 * and the linear system solved is
 *     A1 * Y = Pc*Pr*B1, where B was overwritten by B1 = diag(R)*B, and
 * the permutation to B1 by Pc*Pr is applied internally in this routine.
 * 
 * Arguments
 * =========
 *
 * n      (input) int (global)
 *        The order of the system of linear equations.
 *
 * LUstruct (input) LUstruct_t*
 *        The distributed data structures storing L and U factors.
 *        The L and U factors are obtained from PDGSTRF for
 *        the possibly scaled and permuted matrix A.
 *        See superlu_ddefs.h for the definition of 'LUstruct_t'.
 *        A may be scaled and permuted into A1, so that
 *        A1 = Pc*Pr*diag(R)*A*diag(C)*Pc^T = L*U
 *
 * grid   (input) gridinfo_t*
 *        The 2D process mesh. It contains the MPI communicator, the number
 *        of process rows (NPROW), the number of process columns (NPCOL),
 *        and my process rank. It is an input argument to all the
 *        parallel routines.
 *        Grid can be initialized by subroutine SUPERLU_GRIDINIT.
 *        See superlu_defs.h for the definition of 'gridinfo_t'.
 *
 * B      (input/output) double*
 *        On entry, the distributed right-hand side matrix of the possibly
 *        equilibrated system. That is, B may be overwritten by diag(R)*B.
 *        On exit, the distributed solution matrix Y of the possibly
 *        equilibrated system if info = 0, where Y = Pc*diag(C)^(-1)*X,
 *        and X is the solution of the original system.
 *
 * m_loc  (input) int (local)
 *        The local row dimension of matrix B.
 *
 * fst_row (input) int (global)
 *        The row number of B's first row in the global matrix.
 *
 * ldb    (input) int (local)
 *        The leading dimension of matrix B.
 *
 * nrhs   (input) int (global)
 *        Number of right-hand sides.
 * 
 * SOLVEstruct (input) SOLVEstruct_t* (global)
 *        Contains the information for the communication during the
 *        solution phase.
 *
 * stat   (output) SuperLUStat_t*
 *        Record the statistics about the triangular solves.
 *        See util.h for the definition of 'SuperLUStat_t'.
 *
 * info   (output) int*
 * 	   = 0: successful exit
 *	   < 0: if info = -i, the i-th argument had an illegal value
 * </pre>       
 */

void
pdgstrs(int_t n, LUstruct_t *LUstruct, 
	ScalePermstruct_t *ScalePermstruct,
	gridinfo_t *grid, double *B,
	int_t m_loc, int_t fst_row, int_t ldb, int nrhs,
	SOLVEstruct_t *SOLVEstruct,
	SuperLUStat_t *stat, int *info)
{
    Glu_persist_t *Glu_persist = LUstruct->Glu_persist;
    LocalLU_t *Llu = LUstruct->Llu;
    double alpha = 1.0;
    double beta = 0.0;
    double zero = 0.0;
    double *lsum;  /* Local running sum of the updates to B-components */
    double *x;     /* X component at step k. */
		    /* NOTE: x and lsum are of same size. */
    double *lusup, *dest;
    double *recvbuf,*recvbuf_on, *tempv, *recvbufall, *recvbuf_BC_fwd, *recvbuf0;
    double *rtemp; /* Result of full matrix-vector multiply. */
    double *Linv; /* Inverse of diagonal block */
    double *Uinv; /* Inverse of diagonal block */
	int *ipiv; 
	
    int_t  **Ufstnz_br_ptr = Llu->Ufstnz_br_ptr;
	BcTree  *LBtree_ptr = Llu->LBtree_ptr;
	RdTree  *LRtree_ptr = Llu->LRtree_ptr;
    int_t  *Urbs, *Urbs1; /* Number of row blocks in each block column of U. */
    Ucb_indptr_t **Ucb_indptr;/* Vertical linked list pointing to Uindex[] */
    int_t  **Ucb_valptr;      /* Vertical linked list pointing to Unzval[] */
    int_t  kcol, krow, mycol, myrow;
    int_t  i, ii, il, j, jj, k, lb, ljb, lk, lib, lptr, luptr, gb, nn;
    int_t  nb, nlb,nlb_nodiag, nub, nsupers, nsupers_j, nsupers_i;
    int_t  *xsup, *supno, *lsub, *usub;
    int_t  *ilsum;    /* Starting position of each supernode in lsum (LOCAL)*/
    int    Pc, Pr, iam;
    int    knsupc, nsupr, nprobe;
	int    nbtree, nrtree, outcount;
    int    ldalsum;   /* Number of lsum entries locally owned. */
    int    maxrecvsz, p, pi;
    int_t  **Lrowind_bc_ptr;
    double **Lnzval_bc_ptr;
    double **Linv_bc_ptr;
    double **Uinv_bc_ptr;
    double sum;
    MPI_Status status,status_on,statusx,statuslsum;
    MPI_Request *send_req, recv_req, req;
    pxgstrs_comm_t *gstrs_comm = SOLVEstruct->gstrs_comm;

	double tmax;
	
    /*-- Counts used for L-solve --*/
    int_t  *fmod;         /* Modification count for L-solve --
                             Count the number of local block products to
                             be summed into lsum[lk]. */
    int_t  **fsendx_plist = Llu->fsendx_plist;
    int_t  nfrecvx = Llu->nfrecvx; /* Number of X components to be recv'd. */
    int_t  nfrecvx_buf=0;						 
    int_t  *frecv;        /* Count of lsum[lk] contributions to be received
                             from processes in this row. 
                             It is only valid on the diagonal processes. */
    int_t  nfrecvmod = 0; /* Count of total modifications to be recv'd. */
    int_t  nleaf = 0, nroot = 0;
    int_t  nleaftmp = 0, nroottmp = 0;
	int_t  msgsize;
    /*-- Counts used for U-solve --*/
    int_t  *bmod;         /* Modification count for U-solve. */
    int_t  **bsendx_plist = Llu->bsendx_plist;
    int_t  nbrecvx = Llu->nbrecvx; /* Number of X components to be recv'd. */
    int_t  *brecv;        /* Count of modifications to be recv'd from
			     processes in this row. */
    int_t  nbrecvmod = 0; /* Count of total modifications to be recv'd. */
	int_t flagx,flaglsum,flag;
	int_t *LBTree_active, *LRTree_active, *LBTree_finish, *LRTree_finish, *leafsups; 
	StdList LBList, LRList;
	int_t TAG;
    double t1_sol, t2_sol, t;
#if ( DEBUGlevel>=2 )
    int_t Ublocks = 0;
#endif

    int_t *mod_bit = Llu->mod_bit; /* flag contribution from each row block */
	int INFO;
 
#if ( PROFlevel>=1 )
    double t1, t2;
    float msg_vol = 0, msg_cnt = 0;
#endif 

    int_t *msgcnt=(int_t *) SUPERLU_MALLOC(4 * sizeof(int_t));   /* Count the size of the message xfer'd in each buffer:
		    *     0 : transferred in Lsub_buf[]
		    *     1 : transferred in Lval_buf[]
		    *     2 : transferred in Usub_buf[]
		    *     3 : transferred in Uval_buf[]
		    */
    int iword = sizeof (int_t);
    int dword = sizeof (double);	
 
	yes_no_t done;
	yes_no_t startforward;
 
	int nbrow;
    int_t  ik, rel, idx_r, jb, nrbl, irow, pc,iknsupc;
	int_t  lptr1_tmp, idx_i, idx_v,m; 
 
 
int num_thread = 1;
#ifdef _OPENMP
#pragma omp parallel default(shared)
    {
        if (omp_get_thread_num () == 0) {
            num_thread = omp_get_num_threads ();
        }
    }
#endif
if(iam==0){
 printf("num_thread: %5d\n",num_thread);
 fflush(stdout);
}
 
	MPI_Barrier( grid->comm );
    TIC(t1_sol);
	t = SuperLU_timer_();
 
    /* Test input parameters. */
    *info = 0;
    if ( n < 0 ) *info = -1;
    else if ( nrhs < 0 ) *info = -9;
    if ( *info ) {
	pxerr_dist("PDGSTRS", grid, -*info);
	return;
    }
	
    /*
     * Initialization.
     */
    iam = grid->iam;
    Pc = grid->npcol;
    Pr = grid->nprow;
    myrow = MYROW( iam, grid );
    mycol = MYCOL( iam, grid );
    xsup = Glu_persist->xsup;
    supno = Glu_persist->supno;
    nsupers = supno[n-1] + 1;
    Lrowind_bc_ptr = Llu->Lrowind_bc_ptr;
    Lnzval_bc_ptr = Llu->Lnzval_bc_ptr;
    Linv_bc_ptr = Llu->Linv_bc_ptr;
    Uinv_bc_ptr = Llu->Uinv_bc_ptr;
    nlb = CEILING( nsupers, Pr ); /* Number of local block rows. */

	stat->utime[SOL_COMM] = 0.0;
	stat->utime[SOL_COMM_PROBE] = 0.0;
	stat->utime[SOL_COMM_TESTSOME] = 0.0;
	stat->utime[SOL_GEMM] = 0.0;
	stat->utime[SOL_TRSM] = 0.0;
	stat->utime[SOL_L] = 0.0;
	
	
#if ( DEBUGlevel>=1 )
    CHECK_MALLOC(iam, "Enter pdgstrs()");
#endif

    stat->ops[SOLVE] = 0.0;
    Llu->SolveMsgSent = 0;

    /* Save the count to be altered so it can be used by
       subsequent call to PDGSTRS. */
    if ( !(fmod = intMalloc_dist(nlb)) )
	ABORT("Calloc fails for fmod[].");
    for (i = 0; i < nlb; ++i) fmod[i] = Llu->fmod[i];
    if ( !(frecv = intCalloc_dist(nlb)) )
	ABORT("Malloc fails for frecv[].");
    Llu->frecv = frecv;

    k = SUPERLU_MAX( Llu->nfsendx, Llu->nbsendx ) + nlb;
    if ( !(send_req = (MPI_Request*) SUPERLU_MALLOC(k*sizeof(MPI_Request))) )
	ABORT("Malloc fails for send_req[].");

#ifdef _CRAY
    ftcs1 = _cptofcd("L", strlen("L"));
    ftcs2 = _cptofcd("N", strlen("N"));
    ftcs3 = _cptofcd("U", strlen("U"));
#endif


    /* Obtain ilsum[] and ldalsum for process column 0. */
    ilsum = Llu->ilsum;
    ldalsum = Llu->ldalsum;

    /* Allocate working storage. */
    knsupc = sp_ienv_dist(3);
	nprobe = sp_ienv_dist(4);
    maxrecvsz = knsupc * nrhs + SUPERLU_MAX( XK_H, LSUM_H );
    if ( !(lsum = doubleCalloc_dist(((size_t)ldalsum)*nrhs + nlb*LSUM_H)) )
	ABORT("Calloc fails for lsum[].");
    if ( !(x = doubleCalloc_dist(ldalsum * nrhs + nlb * XK_H)) )
	ABORT("Calloc fails for x[].");
    if ( !(recvbuf = doubleMalloc_dist(maxrecvsz)) )
	ABORT("Malloc fails for recvbuf[].");   
    if ( !(recvbuf_on = doubleMalloc_dist(maxrecvsz)) )
	ABORT("Malloc fails for recvbuf_on[].");   
	// // if ( !(recvbuf_BC_fwd = doubleMalloc_dist(maxrecvsz*nfrecvx)) )   // this needs to be optimized for 1D row mapping
	// // ABORT("Malloc fails for recvbuf_BC_fwd[].");
    if ( !(rtemp = doubleCalloc_dist(ldalsum * nrhs)) )
	ABORT("Malloc fails for rtemp[].");
	if ( !(ipiv = intCalloc_dist(knsupc)) )
	ABORT("Malloc fails for ipiv[].");



    // if ( !(recvbufall = doubleMalloc_dist(n)) )
	// ABORT("Malloc fails for recvbufall[].");

    /*---------------------------------------------------
     * Forward solve Ly = b.
     *---------------------------------------------------*/
    /* Redistribute B into X on the diagonal processes. */
    pdReDistribute_B_to_X(B, m_loc, nrhs, ldb, fst_row, ilsum, x, 
			  ScalePermstruct, Glu_persist, grid, SOLVEstruct);

			  
#if ( PRNTlevel>=1 )
    t = SuperLU_timer_() - t;
    if ( !iam) printf(".. B to X redistribute time\t%8.4f\n", t);
    t = SuperLU_timer_();
#endif			  
			  
			  
			  
    /* Set up the headers in lsum[]. */
    ii = 0;
    for (k = 0; k < nsupers; ++k) {
	knsupc = SuperSize( k );
	krow = PROW( k, grid );
	if ( myrow == krow ) {
	    lk = LBi( k, grid );   /* Local block number. */
	    il = LSUM_BLK( lk );
	    lsum[il - LSUM_H] = k; /* Block number prepended in the header. */
	}
	ii += knsupc;
    }

	
	
    /* ---------------------------------------------------------
       Precompute mapping from Lrowind_bc_ptr to lsum.
       --------------------------------------------------------- */		
    
	nsupers_j = CEILING( nsupers, grid->npcol ); /* Number of local block columns */
	if ( !(Llu->Lrowind_bc_2_lsum = 
              (int_t**)SUPERLU_MALLOC(nsupers_j * sizeof(int_t*))) )
	    ABORT("Malloc fails for Lrowind_bc_2_lsum[].");
	
	
		for (ljb = 0; ljb < nsupers_j; ++ljb) {
			
			
			if(Lrowind_bc_ptr[ljb]!=NULL){
			
				jb = mycol+ljb*grid->npcol;
				
				knsupc = SuperSize( jb );
				krow = PROW( jb, grid );
				nrbl = Lrowind_bc_ptr[ljb][0];
				
				
				if(myrow==krow){ /* skip the diagonal block */
					nlb_nodiag=nrbl-1;
					idx_i = nlb_nodiag+2;
					m = Lrowind_bc_ptr[ljb][1]-knsupc;
				}else{
					nlb_nodiag=nrbl;
					idx_i = nlb_nodiag;
					m = Lrowind_bc_ptr[ljb][1];
				}	
				
				if(nlb_nodiag>0){		
					if ( !(Llu->Lrowind_bc_2_lsum[ljb] = intMalloc_dist(m*nrhs)) )
						ABORT("Malloc fails for Lrowind_bc_2_lsum[ljb][].");	
					idx_r=0;
					RHS_ITERATE(j)
					for (lb = 0; lb < nlb_nodiag; ++lb) {
						lptr1_tmp = Llu->Lindval_loc_bc_ptr[ljb][lb+idx_i];	
						ik = Lrowind_bc_ptr[ljb][lptr1_tmp]; /* Global block number, row-wise. */
						iknsupc = SuperSize( ik );
						nbrow = Lrowind_bc_ptr[ljb][lptr1_tmp+1];
						lk = LBi( ik, grid ); /* Local block number, row-wise. */
						il = LSUM_BLK( lk );
						rel = xsup[ik]; /* Global row index of block ik. */
						for (ii = 0; ii < nbrow; ++ii) {	
							irow = Lrowind_bc_ptr[ljb][lptr1_tmp+LB_DESCRIPTOR+ii] - rel; /* Relative row. */		
							Llu->Lrowind_bc_2_lsum[ljb][idx_r++] = il+irow+ j*iknsupc;	


							// {RHS_ITERATE(j)
							// Llu->Lrowind_bc_2_lsum[ljb][idx_r+j*m] = il+irow+ j*iknsupc;	
							// }
							// idx_r++;							
							
						}			
					}		
				}
			}		
		}	


	
	
	
    /* ---------------------------------------------------------
       Initialize the async Bcast trees on all processes.
       --------------------------------------------------------- */		
	nsupers_j = CEILING( nsupers, grid->npcol ); /* Number of local block columns */
	if ( !(	LBTree_active = intCalloc_dist(nsupers_j)) )
	ABORT("Calloc fails for LBTree_active.");
	LBList = StdList_Init();
	stat->MaxActiveBTrees=0;	
	if ( !(	LBTree_finish = intCalloc_dist(nsupers_j)) )
	ABORT("Calloc fails for LBTree_finish.");	
	
	nbtree = 0;
	for (lk=0;lk<nsupers_j;++lk){
		if(LBtree_ptr[lk]!=NULL){
		if(BcTree_IsRoot(LBtree_ptr[lk])==NO){			
			nbtree++;
			// BcTree_AllocateBuffer(LBtree_ptr[lk]);	
			if(BcTree_getDestCount(LBtree_ptr[lk])>0)nfrecvx_buf++;				  
		}
		BcTree_allocateRequest(LBtree_ptr[lk]);
		}
	}
	
	
	
	nsupers_i = CEILING( nsupers, grid->nprow ); /* Number of local block rows */
	if ( !(	LRTree_active = intCalloc_dist(nsupers_i)) )
	ABORT("Calloc fails for LRTree_active.");	
	LRList = StdList_Init();
	stat->MaxActiveRTrees=0;
	if ( !(	LRTree_finish = intCalloc_dist(nsupers_i)) )
	ABORT("Calloc fails for LBTree_finish.");
	if ( !(	leafsups = (int_t*)intCalloc_dist(nsupers_i)) )
	ABORT("Calloc fails for leafsups.");


	
	// if(iam==8){
		// printf("nsupers_j %5d \n",nsupers_j);
		// fflush(stdout);		
	// for (lk=0;lk<nsupers_j;++lk){
		// lsub = (int_t*)Lrowind_bc_ptr[lk];
		// if(lsub!=NULL){
				// printf("lk %5d lsub[0] %5d \n",lk,lsub[0]);
				// fflush(stdout);			
			// }else{
				// printf("lk %5d lsub null \n",lk);
				// fflush(stdout);				
			// }
			

	// }
	// }
	
	
	
	
	
	
	nrtree = 0;
	nleaf=0;
	for (lk=0;lk<nsupers_i;++lk){
		if(LRtree_ptr[lk]!=NULL){
			nrtree++;
			// RdTree_AllocRecvBuffers(LRtree_ptr[lk]);
			RdTree_allocateRequest(LRtree_ptr[lk]);			
			frecv[lk] = RdTree_GetDestCount(LRtree_ptr[lk]);
			nfrecvmod += frecv[lk];
		}else{
			gb = myrow+lk*grid->nprow;  /* not sure */
			if(gb<nsupers){
			kcol = PCOL( gb, grid );
			if(mycol==kcol) { /* Diagonal process */
			LRTree_active[lk]=2; /* block row lk becomes local*/
			if ( !fmod[lk] ){
			leafsups[nleaf]=gb;				
			++nleaf;
			// if(iam==8){
				// printf("lk %5d gb %5d\n",LBj( gb, grid ),gb);
				// fflush(stdout);
				
			// }
			}
			}
			}
		}
	}	
	
	// printf("%5d ddd am I here!!\n",iam);
	// fflush(stdout);


	if ( !(recvbuf_BC_fwd = doubleMalloc_dist(maxrecvsz*(nfrecvx+1))) )   // this needs to be optimized for 1D row mapping
	ABORT("Malloc fails for recvbuf_BC_fwd[].");	
	nfrecvx_buf=0;			

#if ( DEBUGlevel>=2 )
    printf("(%2d) nfrecvx %4d,  nfrecvmod %4d,  nleaf %4d\n,  nbtree %4d\n,  nrtree %4d\n",
	   iam, nfrecvx, nfrecvmod, nleaf, nbtree, nrtree);
	fflush(stdout);
#endif

	
	
#if ( PRNTlevel>=1 )
    t = SuperLU_timer_() - t;
    if ( !iam) printf(".. Setup L-solve time\t%8.3f\n", t);
    t = SuperLU_timer_();
#endif
	
    /* ---------------------------------------------------------
       Solve the leaf nodes first by all the diagonal processes.
       --------------------------------------------------------- */
#if ( DEBUGlevel>=2 )
    printf("(%2d) nleaf %4d\n", iam, nleaf);
	fflush(stdout);
#endif
    
	
// #if ( PROFlevel>=1 )
		// TIC(t1);
		// msgcnt[1] = maxrecvsz;
// #endif			
			


	// for (jj=0;jj<19*3;jj++){
	// printf("Lindval %5d\n",Llu->Lindval_loc_bc_ptr[0][jj]);
	// fflush(stdout);
	// }			
	
	
	

	// for (k = 0; k < nsupers && nleaf; ++k) {
	for (jj=0;jj<nleaf;jj++){
	k=leafsups[jj];
	// if(iam==8){
		// printf("iam %5d, jj %5d nleaf %5d k %5d\n",iam,jj,nleaf,k);
		// fflush(stdout);
	// }	
	krow = PROW( k, grid );
	kcol = PCOL( k, grid );
	
	// lk = LBi( k, grid );
	// printf("%5d%5d%5d%5d\n",myrow == krow,mycol == kcol,frecv[lk],fmod[lk]);
	// fflush(stdout);
	
	// if ( myrow == krow && mycol == kcol ) { /* Diagonal process */
	    knsupc = SuperSize( k );
	    lk = LBi( k, grid );
		
	    // if ( frecv[lk]==0 && fmod[lk]==0 ) { 
		fmod[lk] = -1;  /* Do not solve X[k] in the future. */
		ii = X_BLK( lk );
		lk = LBj( k, grid ); /* Local block number, column-wise. */
		lsub = Lrowind_bc_ptr[lk];
		lusup = Lnzval_bc_ptr[lk];
		
		// if(iam==8){
			// printf("k %5d lk %5d \n",k,lk);
			// fflush(stdout);
		// }
		
		nsupr = lsub[1];
		

#if ( PROFlevel>=1 )
		TIC(t1);
#endif			
		if(Llu->inv == 1){
		  Linv = Linv_bc_ptr[lk];
#ifdef _CRAY
		  SGEMM( ftcs2, ftcs2, &knsupc, &nrhs, &knsupc,
			  &alpha, Linv, &knsupc, &x[ii],
			  &knsupc, &beta, rtemp, &knsupc );
#elif defined (USE_VENDOR_BLAS)
		  dgemm_( "N", "N", &knsupc, &nrhs, &knsupc,
			   &alpha, Linv, &knsupc, &x[ii],
			   &knsupc, &beta, rtemp, &knsupc, 1, 1 );
#else
		  dgemm_( "N", "N", &knsupc, &nrhs, &knsupc,
			   &alpha, Linv, &knsupc, &x[ii],
			   &knsupc, &beta, rtemp, &knsupc );
#endif		   
		  for (i=0 ; i<knsupc*nrhs ; i++){
			x[ii+i] = rtemp[i];
		  }		
		}else{
#ifdef _CRAY
			STRSM(ftcs1, ftcs1, ftcs2, ftcs3, &knsupc, &nrhs, &alpha,
				  lusup, &nsupr, &x[ii], &knsupc);
#elif defined (USE_VENDOR_BLAS)
			dtrsm_("L", "L", "N", "U", &knsupc, &nrhs, &alpha, 
				lusup, &nsupr, &x[ii], &knsupc, 1, 1, 1, 1);	
#else
			dtrsm_("L", "L", "N", "U", &knsupc, &nrhs, &alpha, 
				   lusup, &nsupr, &x[ii], &knsupc);
#endif
		}
		
		
				// {
				// double temp=0;
				// for (int kk=0;kk<knsupc;kk++){
					// temp = temp + x[ii+kk];
				// }
				// // if(iam==0)
				// printf("leaf Iam %5d, tempsum %10f, nsuper %5d, k, %5d\n",iam,temp,nsupers,k);
							// fflush(stdout);
				// }	
		
#if ( PROFlevel>=1 )
		TOC(t2, t1);
		stat->utime[SOL_TRSM] += t2;
	
#endif	


		stat->ops[SOLVE] += knsupc * (knsupc - 1) * nrhs;
		// --nleaf;
#if ( DEBUGlevel>=2 )
		printf("(%2d) Solve X[%2d]\n", iam, k);
#endif

		/*
		 * Send Xk to process column Pc[k].
		 */
		 
		// if(LBtree_ptr[lk]!=NULL){ 
		// lib = LBi( k, grid ); /* Local block number, row-wise. */
		// ii = X_BLK( lib );
		// BcTree_SetLocalBuffer(LBtree_ptr[lk],&x[ii - XK_H]);
		// BcTree_SetDataReady(LBtree_ptr[lk]);	
		// done = BcTree_Progress(LBtree_ptr[lk]);	
		// assert(done==NO);
		 
		 if(LBtree_ptr[lk]!=NULL){ 
			lib = LBi( k, grid ); /* Local block number, row-wise. */
			ii = X_BLK( lib );		 
			BcTree_forwardMessageSimple(LBtree_ptr[lk],&x[ii - XK_H]);
		 }
		 
		// for (p = 0; p < Pr; ++p) {
		    // if ( fsendx_plist[lk][p] != EMPTY ) {
			// pi = PNUM( p, kcol, grid );
		
			
			// MPI_Isend( &x[ii - XK_H], knsupc * nrhs + XK_H,
				   // MPI_DOUBLE, pi, Xk, grid->comm,
                                   // &send_req[Llu->SolveMsgSent++]);
// #if 0
			// MPI_Send( &x[ii - XK_H], knsupc * nrhs + XK_H,
				 // MPI_DOUBLE, pi, Xk, grid->comm );
// #endif



// #if ( DEBUGlevel>=2 )
			// printf("(%2d) Sent X[%2.0f] to P %2d\n",
			       // iam, x[ii-XK_H], pi);
// #endif
		    // }
		// }
		
		
		

		
		/*
		 * Perform local block modifications: lsum[i] -= L_i,k * X[k]
		 */
		nb = lsub[0] - 1;
		lptr = BC_HEADER + LB_DESCRIPTOR + knsupc;
		luptr = knsupc; /* Skip diagonal block L(k,k). */

		

		
		// printf("%5d Iam in %5d\n",iam,nleaf);	
		// fflush(stdout);
		dlsum_fmod_inv(lsum, x, &x[ii], rtemp, nrhs, knsupc, k,
			   fmod, nb, lptr, luptr, xsup, grid, Llu, 
			   send_req, stat);
		// printf("%5d Iam out %5d",iam,nleaf);	
		// fflush(stdout);
		
		
	    // }
	// } /* if diagonal process ... */
    } /* for k ... */
	


	int data_recv=0;
	int msg_num = nfrecvx+nfrecvmod;
	int nfpost=0,nfrecv=0;
	jj=0;
	recvbuf0 = &recvbuf_BC_fwd[nfrecvx_buf*maxrecvsz];
    /* -----------------------------------------------------------
       Compute the internal nodes asynchronously by all processes.
       ----------------------------------------------------------- */
															 
    while ( nfrecvx || nfrecvmod ) { /* While not finished. */
 
	// printf("iam %5d, nfrecv %5d, num %5d jj %5d nleaf %5d\n",iam,nfrecv,msg_num,jj,nleaf);
	// fflush(stdout); 									 

#if ( PROFlevel>=1 )
		TIC(t1);
		msgcnt[1] = maxrecvsz;
#endif	



	/* Receive a message. */
	MPI_Recv( recvbuf0, maxrecvsz, MPI_DOUBLE,
                  MPI_ANY_SOURCE, MPI_ANY_TAG, grid->comm, &status );		
						 

			  
#if ( PROFlevel>=1 )
		  

		 
																							 
						   
		// if(iam==0){
		// printf("time: %8.5f\n")
		// }
			
					 
		TOC(t2, t1);
		stat->utime[SOL_COMM] += t2;
	   
        msg_cnt += 1;
        msg_vol += msgcnt[1] * dword;			
#endif					  
				  
	  
	k = *recvbuf0;

 
				  
			   
		  
			  

#if ( DEBUGlevel>=2 )
	printf("(%2d) Recv'd block %d, tag %2d\n", iam, k, status.MPI_TAG);
#endif
	
	if(status.MPI_TAG<nsupers){
	      --nfrecvx;
		  
		  // printf("Iam %5d, nfrecvx %5d, nfrecvmod %5d\n",iam,nfrecvx,nfrecvmod);	
		  // fflush(stdout);
		  
		  lk = LBj( status.MPI_TAG, grid );    /* local block number */
		  
		  if(BcTree_getDestCount(LBtree_ptr[lk])>0){
		  
		  // msgsize = SuperSize( status.MPI_TAG )*nrhs+XK_H;	  
		  // for (i = 0; i < msgsize; ++i)
			   // recvbuf_BC_fwd[i + nfrecvx_buf*maxrecvsz] = recvbuf0[i];	
		   
		   // BcTree_forwardMessageSimple(LBtree_ptr[lk],&recvbuf_BC_fwd[nfrecvx_buf*maxrecvsz]);
		   // nfrecvx_buf++;
		   
		   
		  
														 
		  
		 BcTree_forwardMessageSimple(LBtree_ptr[lk],recvbuf0);	
		 nfrecvx_buf++;
		  }
		  
		  
	      lk = LBj( k, grid ); /* Local block number, column-wise. */
	      lsub = Lrowind_bc_ptr[lk];
	      lusup = Lnzval_bc_ptr[lk];
	      if ( lsub ) {
		  nb   = lsub[0];
		  lptr = BC_HEADER;
		  luptr = 0;
		  knsupc = SuperSize( k );

	   
		  /*
		   * Perform local block modifications: lsum[i] -= L_i,k * X[k]
		   */
		  dlsum_fmod_inv(lsum, x, &recvbuf0[XK_H], rtemp, nrhs, knsupc, k,
			     fmod, nb, lptr, luptr, xsup, grid, Llu, 
			     send_req, stat);
	      } /* if lsub */

	
		 //  BcTree_waitSendRequest(LBtree_ptr[lk]);	
		  if(BcTree_getDestCount(LBtree_ptr[lk])>0){
		  recvbuf0 = &recvbuf_BC_fwd[nfrecvx_buf*maxrecvsz];
		  }

	}else if(status.MPI_TAG>=nsupers && status.MPI_TAG<nsupers*2){
	      --nfrecvmod;		  
		  lk = LBi( k, grid ); /* Local block number, row-wise. */
	      --frecv[lk];
		  
		  if(RdTree_IsRoot(LRtree_ptr[lk])==YES){
			  ii = X_BLK( lk );
			  knsupc = SuperSize( k );
			  tempv = &recvbuf0[LSUM_H];
			  RHS_ITERATE(j) {
			  for (i = 0; i < knsupc; ++i)
				  x[i + ii + j*knsupc] += tempv[i + j*knsupc];
			  }			
			
		      if ( (frecv[lk])==0 && fmod[lk]==0 ) {
			  
				  fmod[lk] = -1; /* Do not solve X[k] in the future. */
				  lk = LBj( k, grid ); /* Local block number, column-wise. */
				  lsub = Lrowind_bc_ptr[lk];
				  lusup = Lnzval_bc_ptr[lk];
				  nsupr = lsub[1];
				  
				  
			
		#if ( PROFlevel>=1 )
				TIC(t1);
		#endif			  
				  

				if(Llu->inv == 1){
				  Linv = Linv_bc_ptr[lk];		  
		#ifdef _CRAY
				  SGEMM( ftcs2, ftcs2, &knsupc, &nrhs, &knsupc,
					  &alpha, Linv, &knsupc, &x[ii],
					  &knsupc, &beta, rtemp, &knsupc );
		#elif defined (USE_VENDOR_BLAS)
				  dgemm_( "N", "N", &knsupc, &nrhs, &knsupc,
					   &alpha, Linv, &knsupc, &x[ii],
					   &knsupc, &beta, rtemp, &knsupc, 1, 1 );
		#else
				  dgemm_( "N", "N", &knsupc, &nrhs, &knsupc,
					   &alpha, Linv, &knsupc, &x[ii],
					   &knsupc, &beta, rtemp, &knsupc );
		#endif			   
				  for (i=0 ; i<knsupc*nrhs ; i++){
					x[ii+i] = rtemp[i];
				  }		
				}
				else{
		#ifdef _CRAY
				  STRSM(ftcs1, ftcs1, ftcs2, ftcs3, &knsupc, &nrhs, &alpha,
					lusup, &nsupr, &x[ii], &knsupc);
		#elif defined (USE_VENDOR_BLAS)
				  dtrsm_("L", "L", "N", "U", &knsupc, &nrhs, &alpha, 
					lusup, &nsupr, &x[ii], &knsupc, 1, 1, 1, 1);		
		#else
				  dtrsm_("L", "L", "N", "U", &knsupc, &nrhs, &alpha, 
					 lusup, &nsupr, &x[ii], &knsupc);
		#endif
				}


		#if ( PROFlevel>=1 )
				TOC(t2, t1);
				stat->utime[SOL_TRSM] += t2;
			
		#endif	



				  stat->ops[SOLVE] += knsupc * (knsupc - 1) * nrhs;
		#if ( DEBUGlevel>=2 )
				  printf("(%2d) Solve X[%2d]\n", iam, k);
		#endif
				
				  /*
				   * Send Xk to process column Pc[k].
				   */
				  // // // kcol = PCOL( k, grid );
				  // // // for (p = 0; p < Pr; ++p) {
					  // // // if ( fsendx_plist[lk][p] != EMPTY ) {
					  // // // pi = PNUM( p, kcol, grid );

			
					  
					  // // // MPI_Isend( &x[ii-XK_H], knsupc * nrhs + XK_H,
											 // // // MPI_DOUBLE, pi, Xk, grid->comm,
											 // // // &send_req[Llu->SolveMsgSent++]);
		// // // #if 0
					  // // // MPI_Send( &x[ii - XK_H], knsupc * nrhs + XK_H,
							// // // MPI_DOUBLE, pi, Xk, grid->comm );
		// // // #endif


				



		// // // #if ( DEBUGlevel>=2 )
					  // // // printf("(%2d) Sent X[%2.0f] to P %2d\n",
						 // // // iam, x[ii-XK_H], pi);
		// // // #endif
					  // // // }
						  // // // }
						  
					if(LBtree_ptr[lk]!=NULL){ 
						BcTree_forwardMessageSimple(LBtree_ptr[lk],&x[ii - XK_H]);
					}		  
							
						  
				  /*
				   * Perform local block modifications.
				   */
				  nb = lsub[0] - 1;
				  lptr = BC_HEADER + LB_DESCRIPTOR + knsupc;
				  luptr = knsupc; /* Skip diagonal block L(k,k). */

	  
				  dlsum_fmod_inv(lsum, x, &x[ii], rtemp, nrhs, knsupc, k,
						 fmod, nb, lptr, luptr, xsup, grid, Llu,
						 send_req, stat);
	   
			  }
			
		  }else{

			  il = LSUM_BLK( lk );		  
			  knsupc = SuperSize( k );
			  tempv = &recvbuf0[LSUM_H];
			  RHS_ITERATE(j) {
			  for (i = 0; i < knsupc; ++i)
				  lsum[i + il + j*knsupc] += tempv[i + j*knsupc];
			  }						
		      if ( (frecv[lk])==0 && fmod[lk]==0 ) {
				 fmod[lk] = -1; 
				 RdTree_forwardMessageSimple(LRtree_ptr[lk],&lsum[il-LSUM_H]); 
			  }
		  }  
		  
		  // nfrecvx_buf++;
		  recvbuf0 = &recvbuf_BC_fwd[nfrecvx_buf*maxrecvsz];
		  
		  
							
	  } /* check Tag */		  
    } /* while not finished ... */
#if ( PRNTlevel>=1 )
    t = SuperLU_timer_() - t;
	stat->utime[SOL_L] = t;
    if ( !iam ) printf(".. L-solve time\t%8.3f\n", t);
	

    MPI_Reduce (&t, &tmax, 1, MPI_DOUBLE,
		MPI_MAX, 0, grid->comm);
    if ( !iam ) printf(".. L-solve time (MAX) \t%8.3f\n", tmax);	
	
	
	
    t = SuperLU_timer_();
#endif

#if ( DEBUGlevel==2 )
    {
      printf("(%d) .. After L-solve: y =\n", iam);
      for (i = 0, k = 0; k < nsupers; ++k) {
	  krow = PROW( k, grid );
	  kcol = PCOL( k, grid );
	  if ( myrow == krow && mycol == kcol ) { /* Diagonal process */
	      knsupc = SuperSize( k );
	      lk = LBi( k, grid );
	      ii = X_BLK( lk );
	      for (j = 0; j < knsupc; ++j)
		printf("\t(%d)\t%4d\t%.10f\n", iam, xsup[k]+j, x[ii+j]);
	      fflush(stdout);
	  }
	  MPI_Barrier( grid->comm );
      }
    }
#endif

    SUPERLU_FREE(fmod);
    SUPERLU_FREE(frecv);


    /*for (i = 0; i < Llu->SolveMsgSent; ++i) MPI_Request_free(&send_req[i]);*/

    // for (i = 0; i < Llu->SolveMsgSent; ++i) MPI_Wait(&send_req[i], &status);
    // Llu->SolveMsgSent = 0;

	
	for (lk=0;lk<nsupers_j;++lk){
		if(LBtree_ptr[lk]!=NULL){
		// if(BcTree_IsRoot(LBtree_ptr[lk])==YES){			
			BcTree_waitSendRequest(LBtree_ptr[lk]);		
		// }
		// deallocate requests here
		}
	}
	
	for (lk=0;lk<nsupers_i;++lk){
		if(LRtree_ptr[lk]!=NULL){		
			RdTree_waitSendRequest(LRtree_ptr[lk]);		
		// deallocate requests here
		}
	}		
    MPI_Barrier( grid->comm );


    /*---------------------------------------------------
     * Back solve Ux = y.
     *
     * The Y components from the forward solve is already
     * on the diagonal processes.
     *---------------------------------------------------*/

    /* Save the count to be altered so it can be used by
       subsequent call to PDGSTRS. */
    if ( !(bmod = intMalloc_dist(nlb)) )
	ABORT("Calloc fails for bmod[].");
    for (i = 0; i < nlb; ++i) bmod[i] = Llu->bmod[i];
    if ( !(brecv = intMalloc_dist(nlb)) )
	ABORT("Malloc fails for brecv[].");
    Llu->brecv = brecv;

    /*
     * Compute brecv[] and nbrecvmod counts on the diagonal processes.
     */
    {
	superlu_scope_t *scp = &grid->rscp;

#if 1
	for (k = 0; k < nlb; ++k) mod_bit[k] = 0;
	for (k = 0; k < nsupers; ++k) {
	    krow = PROW( k, grid );
	    if ( myrow == krow ) {
		lk = LBi( k, grid );    /* local block number */
		kcol = PCOL( k, grid ); /* root process in this row scope */
		if ( mycol != kcol && bmod[lk] )
		    mod_bit[lk] = 1;  /* Contribution from off-diagonal */
	    }
	}

	/* Every process receives the count, but it is only useful on the
	   diagonal processes.  */

	MPI_Allreduce( mod_bit, brecv, nlb, mpi_int_t, MPI_SUM, scp->comm );
	// MPI_Iallreduce( mod_bit, brecv, nlb, mpi_int_t, MPI_SUM, scp->comm, &req);
	// MPI_Wait(&req,&status);
		
	for (k = 0; k < nsupers; ++k) {
	    krow = PROW( k, grid );
	    if ( myrow == krow ) {
		lk = LBi( k, grid );    /* local block number */
		kcol = PCOL( k, grid ); /* root process in this row scope. */
		if ( mycol == kcol ) { /* diagonal process */
		    nbrecvmod += brecv[lk];
		    if ( !brecv[lk] && !bmod[lk] ) ++nroot;
#if ( DEBUGlevel>=2 )
		    printf("(%2d) brecv[%4d]  %2d\n", iam, k, brecv[lk]);
		    assert( brecv[lk] < Pc );
#endif
		}
	    }
	}

#else /* old */

	for (k = 0; k < nsupers; ++k) {
	    krow = PROW( k, grid );
	    if ( myrow == krow ) {
		lk = LBi( k, grid );    /* Local block number. */
		kcol = PCOL( k, grid ); /* Root process in this row scope. */
		if ( mycol != kcol && bmod[lk] )
		    i = 1;  /* Contribution from non-diagonal process. */
		else i = 0;
		MPI_Reduce( &i, &brecv[lk], 1, mpi_int_t,
			   MPI_SUM, kcol, scp->comm );
		if ( mycol == kcol ) { /* Diagonal process. */
		    nbrecvmod += brecv[lk];
		    if ( !brecv[lk] && !bmod[lk] ) ++nroot;
#if ( DEBUGlevel>=2 )
		    printf("(%2d) brecv[%4d]  %2d\n", iam, k, brecv[lk]);
		    assert( brecv[lk] < Pc );
#endif
		}
	    }
	}
#endif
    }

    /* Re-initialize lsum to zero. Each block header is already in place. */
    for (k = 0; k < nsupers; ++k) {
	krow = PROW( k, grid );
	if ( myrow == krow ) {
	    knsupc = SuperSize( k );
	    lk = LBi( k, grid );
	    il = LSUM_BLK( lk );
	    dest = &lsum[il];
	    RHS_ITERATE(j) {
		for (i = 0; i < knsupc; ++i) dest[i + j*knsupc] = zero;
	    }
	}
    }

    /* Set up additional pointers for the index and value arrays of U.
       nub is the number of local block columns. */
    nub = CEILING( nsupers, Pc ); /* Number of local block columns. */
    if ( !(Urbs = (int_t *) intCalloc_dist(2*nub)) )
	ABORT("Malloc fails for Urbs[]"); /* Record number of nonzero
					     blocks in a block column. */
    Urbs1 = Urbs + nub;
    if ( !(Ucb_indptr = SUPERLU_MALLOC(nub * sizeof(Ucb_indptr_t *))) )
        ABORT("Malloc fails for Ucb_indptr[]");
    if ( !(Ucb_valptr = SUPERLU_MALLOC(nub * sizeof(int_t *))) )
        ABORT("Malloc fails for Ucb_valptr[]");

    /* Count number of row blocks in a block column. 
       One pass of the skeleton graph of U. */
    for (lk = 0; lk < nlb; ++lk) {
	usub = Ufstnz_br_ptr[lk];
	if ( usub ) { /* Not an empty block row. */
	    /* usub[0] -- number of column blocks in this block row. */
#if ( DEBUGlevel>=2 )
	    Ublocks += usub[0];
#endif
	    i = BR_HEADER; /* Pointer in index array. */
	    for (lb = 0; lb < usub[0]; ++lb) { /* For all column blocks. */
		k = usub[i];            /* Global block number */
		++Urbs[LBj(k,grid)];
		i += UB_DESCRIPTOR + SuperSize( k );
	    }
	}
    }

    /* Set up the vertical linked lists for the row blocks.
       One pass of the skeleton graph of U. */
    for (lb = 0; lb < nub; ++lb) {
	if ( Urbs[lb] ) { /* Not an empty block column. */
	    if ( !(Ucb_indptr[lb]
		   = SUPERLU_MALLOC(Urbs[lb] * sizeof(Ucb_indptr_t))) )
		ABORT("Malloc fails for Ucb_indptr[lb][]");
	    if ( !(Ucb_valptr[lb] = (int_t *) intMalloc_dist(Urbs[lb])) )
		ABORT("Malloc fails for Ucb_valptr[lb][]");
	}
    }
    for (lk = 0; lk < nlb; ++lk) { /* For each block row. */
	usub = Ufstnz_br_ptr[lk];
	if ( usub ) { /* Not an empty block row. */
	    i = BR_HEADER; /* Pointer in index array. */
	    j = 0;         /* Pointer in nzval array. */
	    for (lb = 0; lb < usub[0]; ++lb) { /* For all column blocks. */
		k = usub[i];          /* Global block number, column-wise. */
		ljb = LBj( k, grid ); /* Local block number, column-wise. */
		Ucb_indptr[ljb][Urbs1[ljb]].lbnum = lk;
		Ucb_indptr[ljb][Urbs1[ljb]].indpos = i;
		Ucb_valptr[ljb][Urbs1[ljb]] = j;
		++Urbs1[ljb];
		j += usub[i+1];
		i += UB_DESCRIPTOR + SuperSize( k );
	    }
	}
    }

#if ( DEBUGlevel>=2 )
    for (p = 0; p < Pr*Pc; ++p) {
	if (iam == p) {
	    printf("(%2d) .. Ublocks %d\n", iam, Ublocks);
	    for (lb = 0; lb < nub; ++lb) {
		printf("(%2d) Local col %2d: # row blocks %2d\n",
		       iam, lb, Urbs[lb]);
		if ( Urbs[lb] ) {
		    for (i = 0; i < Urbs[lb]; ++i)
			printf("(%2d) .. row blk %2d:\
                               lbnum %d, indpos %d, valpos %d\n",
			       iam, i, 
			       Ucb_indptr[lb][i].lbnum,
			       Ucb_indptr[lb][i].indpos,
			       Ucb_valptr[lb][i]);
		}
	    }
	}
	MPI_Barrier( grid->comm );
    }
    for (p = 0; p < Pr*Pc; ++p) {
	if ( iam == p ) {
	    printf("\n(%d) bsendx_plist[][]", iam);
	    for (lb = 0; lb < nub; ++lb) {
		printf("\n(%d) .. local col %2d: ", iam, lb);
		for (i = 0; i < Pr; ++i)
		    printf("%4d", bsendx_plist[lb][i]);
	    }
	    printf("\n");
	}
	MPI_Barrier( grid->comm );
    }
#endif /* DEBUGlevel */


#if ( PRNTlevel>=1 )
    t = SuperLU_timer_() - t;
    if ( !iam) printf(".. Setup U-solve time\t%8.3f\n", t);
    t = SuperLU_timer_();
#endif

    /*
     * Solve the roots first by all the diagonal processes.
     */
#if ( DEBUGlevel>=2 )
    printf("(%2d) nroot %4d\n", iam, nroot);
	fflush(stdout);				
#endif
    for (k = nsupers-1; k >= 0 && nroot; --k) {
	krow = PROW( k, grid );
	kcol = PCOL( k, grid );
	if ( myrow == krow && mycol == kcol ) { /* Diagonal process. */
	    knsupc = SuperSize( k );
	    lk = LBi( k, grid ); /* Local block number, row-wise. */
	    if ( brecv[lk]==0 && bmod[lk]==0 ) {
		bmod[lk] = -1;       /* Do not solve X[k] in the future. */
		ii = X_BLK( lk );
		lk = LBj( k, grid ); /* Local block number, column-wise */
		lsub = Lrowind_bc_ptr[lk];
		lusup = Lnzval_bc_ptr[lk];
		nsupr = lsub[1];
		
		
		if(Llu->inv == 1){
		
		  Uinv = Uinv_bc_ptr[lk];
#ifdef _CRAY
		  SGEMM( ftcs2, ftcs2, &knsupc, &nrhs, &knsupc,
			  &alpha, Uinv, &knsupc, &x[ii],
			  &knsupc, &beta, rtemp, &knsupc );
#elif defined (USE_VENDOR_BLAS)
		  dgemm_( "N", "N", &knsupc, &nrhs, &knsupc,
			   &alpha, Uinv, &knsupc, &x[ii],
			   &knsupc, &beta, rtemp, &knsupc, 1, 1 );
#else
		  dgemm_( "N", "N", &knsupc, &nrhs, &knsupc,
			   &alpha, Uinv, &knsupc, &x[ii],
			   &knsupc, &beta, rtemp, &knsupc );
#endif			   
		   
		  for (i=0 ; i<knsupc*nrhs ; i++){
			x[ii+i] = rtemp[i];
		  }		
		}else{
#ifdef _CRAY
		  STRSM(ftcs1, ftcs3, ftcs2, ftcs2, &knsupc, &nrhs, &alpha,
		      lusup, &nsupr, &x[ii], &knsupc);
#elif defined (USE_VENDOR_BLAS)
		  dtrsm_("L", "U", "N", "N", &knsupc, &nrhs, &alpha, 
		       lusup, &nsupr, &x[ii], &knsupc, 1, 1, 1, 1);	
#else
		  dtrsm_("L", "U", "N", "N", &knsupc, &nrhs, &alpha, 
		       lusup, &nsupr, &x[ii], &knsupc);
#endif
		}		
		
		

		stat->ops[SOLVE] += knsupc * (knsupc + 1) * nrhs;
		--nroot;
#if ( DEBUGlevel>=2 )
		printf("(%2d) Solve X[%2d]\n", iam, k);
#endif
		/*
		 * Send Xk to process column Pc[k].
		 */
		for (p = 0; p < Pr; ++p) {
		    if ( bsendx_plist[lk][p] != EMPTY ) {
			pi = PNUM( p, kcol, grid );



// #if ( PROFlevel>=1 )
		// TIC(t1);
		// msgcnt[1] = knsupc * nrhs + XK_H;
// #endif	

			MPI_Isend( &x[ii - XK_H], knsupc * nrhs + XK_H,
                                   MPI_DOUBLE, pi, Xk, grid->comm,
                                   &send_req[Llu->SolveMsgSent++]);
#if 0
			MPI_Send( &x[ii - XK_H], knsupc * nrhs + XK_H,
                                  MPI_DOUBLE, pi, Xk,
                                  grid->comm );
#endif


// #if ( PROFlevel>=1 )
		// TOC(t2, t1);
		// stat->utime[SOL_COMM] += t2;
	
        // msg_cnt += 1;
        // msg_vol += msgcnt[1] * dword;		
// #endif	


#if ( DEBUGlevel>=2 )
			printf("(%2d) Sent X[%2.0f] to P %2d\n",
			       iam, x[ii-XK_H], pi);
#endif
		    }
		}
		/*
		 * Perform local block modifications: lsum[i] -= U_i,k * X[k]
		 */
		if ( Urbs[lk] ) 
		    dlsum_bmod_inv(lsum, x, &x[ii], rtemp, nrhs, k, bmod, Urbs,
			       Ucb_indptr, Ucb_valptr, xsup, grid, Llu,
			       send_req, stat);
	    } /* if root ... */
	} /* if diagonal process ... */
    } /* for k ... */


    /*
     * Compute the internal nodes asychronously by all processes.
     */
	 
// printf("nbrecvx %5d nbrecvmod %5d\n",nbrecvx,nbrecvmod);
// fflush(stdout);
 
	 while ( nbrecvx || nbrecvmod ) { /* While not finished. */

			  
// #if ( PROFlevel>=1 )
		// TIC(t1);
		// msgcnt[1] = maxrecvsz;
// #endif	
	
	/* Receive a message. */
	MPI_Recv( recvbuf, maxrecvsz, MPI_DOUBLE,
                  MPI_ANY_SOURCE, MPI_ANY_TAG, grid->comm, &status );
				  
// #if ( PROFlevel>=1 )
		// TOC(t2, t1);
		// stat->utime[SOL_COMM] += t2;
	
        // msg_cnt += 1;
        // msg_vol += msgcnt[1] * dword;		
// #endif					  
				  	  
	k = *recvbuf;

#if ( DEBUGlevel>=2 )
	printf("(%2d) Recv'd block %d, tag %2d\n", iam, k, status.MPI_TAG);
#endif

	switch ( status.MPI_TAG ) {
	    case Xk:
	        --nbrecvx;
		lk = LBj( k, grid ); /* Local block number, column-wise. */
		/*
		 * Perform local block modifications:
		 *         lsum[i] -= U_i,k * X[k]
		 */
		dlsum_bmod_inv(lsum, x, &recvbuf[XK_H], rtemp, nrhs, k, bmod, Urbs,
			   Ucb_indptr, Ucb_valptr, xsup, grid, Llu, 
			   send_req, stat);

	        break;

	    case LSUM: /* Receiver must be a diagonal process */
		--nbrecvmod;
		lk = LBi( k, grid ); /* Local block number, row-wise. */
		ii = X_BLK( lk );
		knsupc = SuperSize( k );
		tempv = &recvbuf[LSUM_H];
		RHS_ITERATE(j) {
		    for (i = 0; i < knsupc; ++i)
			x[i + ii + j*knsupc] += tempv[i + j*knsupc];
		}

		if ( (--brecv[lk])==0 && bmod[lk]==0 ) {
		    bmod[lk] = -1; /* Do not solve X[k] in the future. */
		    lk = LBj( k, grid ); /* Local block number, column-wise. */
		    lsub = Lrowind_bc_ptr[lk];
		    lusup = Lnzval_bc_ptr[lk];
		    nsupr = lsub[1];


		if(Llu->inv == 1){
		
		  Uinv = Uinv_bc_ptr[lk];
		  
#ifdef _CRAY
		  SGEMM( ftcs2, ftcs2, &knsupc, &nrhs, &knsupc,
			  &alpha, Uinv, &knsupc, &x[ii],
			  &knsupc, &beta, rtemp, &knsupc );
#elif defined (USE_VENDOR_BLAS)
		  dgemm_( "N", "N", &knsupc, &nrhs, &knsupc,
			   &alpha, Uinv, &knsupc, &x[ii],
			   &knsupc, &beta, rtemp, &knsupc, 1, 1 );
#else
		  dgemm_( "N", "N", &knsupc, &nrhs, &knsupc,
			   &alpha, Uinv, &knsupc, &x[ii],
			   &knsupc, &beta, rtemp, &knsupc );
#endif		
		   
		  for (i=0 ; i<knsupc*nrhs ; i++){
			x[ii+i] = rtemp[i];
		  }		
		}else{
#ifdef _CRAY
		    STRSM(ftcs1, ftcs3, ftcs2, ftcs2, &knsupc, &nrhs, &alpha,
			  lusup, &nsupr, &x[ii], &knsupc);
#elif defined (USE_VENDOR_BLAS)
		    dtrsm_("L", "U", "N", "N", &knsupc, &nrhs, &alpha, 
		       lusup, &nsupr, &x[ii], &knsupc, 1, 1, 1, 1);		
#else
		    dtrsm_("L", "U", "N", "N", &knsupc, &nrhs, &alpha, 
			   lusup, &nsupr, &x[ii], &knsupc);
#endif
		}

		    stat->ops[SOLVE] += knsupc * (knsupc + 1) * nrhs;
#if ( DEBUGlevel>=2 )
		    printf("(%2d) Solve X[%2d]\n", iam, k);
#endif
		    /*
		     * Send Xk to process column Pc[k].
		     */
		    kcol = PCOL( k, grid );
		    for (p = 0; p < Pr; ++p) {
			if ( bsendx_plist[lk][p] != EMPTY ) {
			    pi = PNUM( p, kcol, grid );

// #if ( PROFlevel>=1 )
		// TIC(t1);
		// msgcnt[1] = knsupc * nrhs + XK_H;
// #endif	

			    MPI_Isend( &x[ii - XK_H], knsupc * nrhs + XK_H,
                                       MPI_DOUBLE, pi, Xk, grid->comm,
                                       &send_req[Llu->SolveMsgSent++] );
#if 0
			    MPI_Send( &x[ii - XK_H], knsupc * nrhs + XK_H,
                                      MPI_DOUBLE, pi, Xk,
                                      grid->comm );
#endif

// #if ( PROFlevel>=1 )
		// TOC(t2, t1);
		// stat->utime[SOL_COMM] += t2;
	
        // msg_cnt += 1;
        // msg_vol += msgcnt[1] * dword;		
// #endif	

#if ( DEBUGlevel>=2 )
			    printf("(%2d) Sent X[%2.0f] to P %2d\n",
				   iam, x[ii - XK_H], pi);
#endif
			}
		    }
		    /*
		     * Perform local block modifications: 
		     *         lsum[i] -= U_i,k * X[k]
		     */
		    if ( Urbs[lk] )
			dlsum_bmod_inv(lsum, x, &x[ii], rtemp, nrhs, k, bmod, Urbs,
				   Ucb_indptr, Ucb_valptr, xsup, grid, Llu,
				   send_req, stat);
		} /* if becomes solvable */
		
		break;

#if ( DEBUGlevel>=2 )
	      default:
		printf("(%2d) Recv'd wrong message tag %4d\n", iam, status.MPI_TAG);
		break;
#endif		

	} /* switch */

    } /* while not finished ... */

#if ( PRNTlevel>=1 )
    t = SuperLU_timer_() - t;
    if ( !iam ) printf(".. U-solve time\t%8.3f\n", t);
    t = SuperLU_timer_();	
#endif

#if ( DEBUGlevel>=2 )
    {
	double *x_col;
	int diag;
	printf("\n(%d) .. After U-solve: x (ON DIAG PROCS) = \n", iam);
	ii = 0;
	for (k = 0; k < nsupers; ++k) {
	    knsupc = SuperSize( k );
	    krow = PROW( k, grid );
	    kcol = PCOL( k, grid );
	    diag = PNUM( krow, kcol, grid);
	    if ( iam == diag ) { /* Diagonal process. */
		lk = LBi( k, grid );
		jj = X_BLK( lk );
		x_col = &x[jj];
		RHS_ITERATE(j) {
		    for (i = 0; i < knsupc; ++i) { /* X stored in blocks */
			printf("\t(%d)\t%4d\t%.10f\n",
			       iam, xsup[k]+i, x_col[i]);
		    }
		    x_col += knsupc;
		}
	    }
	    ii += knsupc;
	} /* for k ... */
    }
#endif

    pdReDistribute_X_to_B(n, B, m_loc, ldb, fst_row, nrhs, x, ilsum,
			  ScalePermstruct, Glu_persist, grid, SOLVEstruct);


#if ( PRNTlevel>=1 )
    t = SuperLU_timer_() - t;
    if ( !iam) printf(".. X to B redistribute time\t%8.4f\n", t);
    t = SuperLU_timer_();
#endif	
			  
			  

    /* Deallocate storage. */
	SUPERLU_FREE(rtemp);
    SUPERLU_FREE(lsum);
    SUPERLU_FREE(x);
    SUPERLU_FREE(recvbuf);
    for (i = 0; i < nub; ++i) {
	if ( Urbs[i] ) {
	    SUPERLU_FREE(Ucb_indptr[i]);
	    SUPERLU_FREE(Ucb_valptr[i]);
	}
    }
    SUPERLU_FREE(Ucb_indptr);
    SUPERLU_FREE(Ucb_valptr);
    SUPERLU_FREE(Urbs);
    SUPERLU_FREE(bmod);
    SUPERLU_FREE(brecv);

    /*for (i = 0; i < Llu->SolveMsgSent; ++i) MPI_Request_free(&send_req[i]);*/

    for (i = 0; i < Llu->SolveMsgSent; ++i) MPI_Wait(&send_req[i], &status);
    SUPERLU_FREE(send_req);

    MPI_Barrier( grid->comm );

	
#if ( PROFlevel>=2 )
    {
        float msg_vol_max, msg_vol_sum, msg_cnt_max, msg_cnt_sum;

        MPI_Reduce (&msg_cnt, &msg_cnt_sum,
                    1, MPI_FLOAT, MPI_SUM, 0, grid->comm);
        MPI_Reduce (&msg_cnt, &msg_cnt_max,
                    1, MPI_FLOAT, MPI_MAX, 0, grid->comm);
        MPI_Reduce (&msg_vol, &msg_vol_sum,
                    1, MPI_FLOAT, MPI_SUM, 0, grid->comm);
        MPI_Reduce (&msg_vol, &msg_vol_max,
                    1, MPI_FLOAT, MPI_MAX, 0, grid->comm);
        if (!iam) {
            printf ("\tPDGSTRS comm stat:"
                    "\tAvg\tMax\t\tAvg\tMax\n"
                    "\t\t\tCount:\t%.0f\t%.0f\tVol(MB)\t%.2f\t%.2f\n",
                    msg_cnt_sum / Pr / Pc, msg_cnt_max,
                    msg_vol_sum / Pr / Pc * 1e-6, msg_vol_max * 1e-6);
        }
    }
#endif	
	
	TOC(t2_sol,t1_sol);
    stat->utime[SOLVE] = t2_sol;

#if ( DEBUGlevel>=1 )
    CHECK_MALLOC(iam, "Exit pdgstrs()");
#endif

    return;
} /* PDGSTRS */

