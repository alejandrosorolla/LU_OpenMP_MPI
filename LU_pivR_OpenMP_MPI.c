#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <mpi.h>
#include <omp.h>

#define Aref(a1,a2)  A[ (a1-1)*(Alda)+(a2-1) ]
#define xref(a1)     x[ (a1-1) ]
#define bref(a1)     b[ (a1-1) ]
#define dabs(a)      ( (a) > 0.0 ? (a) : -(a) )
#define dref(a1)     vPiv[ (a1-1) ]
#define TAG 53
#define ROOT 0

extern double dclock(void);
extern int    read_data(char*, int*, int*, int*, double*, int*, double*, double*);
extern int    print_matrix(char*, int, int, double*, int);
extern int    print_vector(char*, int, double*);
extern int    print_ivector(char*, int, int*);
extern int    copy_matrix(int, int, double*, int, double*, int);
extern int    generate_matrix(int, int, double*, int, double, double);
extern int    generate_matrix_random(int, int, double*, int);
extern int    copy_vector(int, double*, double*);
extern int    generate_vector(int, double*, double, double);
extern int    generate_vector_random(int, double*);
extern int    generate_ivector(int, int*, int, int);
extern int    matrix_vector_product(int, int, double*, int, double*, double*);
extern double compute_error(int m, double* x, double* y);

int main(int argc, char* argv[])
{
	double* Af = NULL, * A = NULL, * xf = NULL, * x = NULL, * bf = NULL, * b = NULL, init, incr, * vtemp;
	double t1, t2, time, flops, tmin, piv;
	double timeLU = 0.0, timeTr = 0.0, GFLOPsLU, GFLOPsTr;
	int    i, j, k, nreps, info, m, n, visual, random, Alda, myId, size, bloque, n_bloque, rowsToReceive;
	int dim, iPiv, processWorking, processWorkingA, mpiGranted;
	int* vPiv = NULL, * sendFromA, * sizeToSendA, startFrom;
	MPI_Status st;

	MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &mpiGranted);
	MPI_Comm_rank(MPI_COMM_WORLD, &myId);
	MPI_Comm_size(MPI_COMM_WORLD, &size);

	if (mpiGranted < MPI_THREAD_FUNNELED) {
        if (myId == ROOT) {
            printf("MPI_THREAD_FUNNELED not granted!\n");
        }
        MPI_Finalize();
        exit(-1);
    }
	
	if (myId == ROOT) {
		printf("----------------------------------------------------------\n");
		printf("Driver for the evaluation of LU factorization routines\n");
		printf("SIE032. Paralelismo, Clusters y Computación GRID\n");
		printf("Universidad Jaime I de Castellón\n");
		printf("October, 2019\n");
		printf("----------------------------------------------------------\n");
		printf("Program starts...\n");
		printf("-->Read data\n");
	}


	info = read_data("main.in", &m, &n, &visual, &tmin, &random, &init, &incr);
	if (info != 0) {
		printf("Error in data read\n");
		exit(-1);
	}

	if (n > m) {
		if (myId == ROOT) {
			printf("This algorithm does not work for incompatible systems\n");
		}
		MPI_Finalize();
		exit(-1);
	}

	/* Allocate space for data */
	Af = (double*)malloc(m * n * sizeof(double));   Alda = m;
	A = (double*)malloc(m * n * sizeof(double));
	xf = (double*)malloc(n * sizeof(double));
	x = (double*)malloc(n * sizeof(double));
	bf = (double*)malloc(m * sizeof(double));
	b = (double*)malloc(m * sizeof(double));
	vPiv = (int*)malloc(m * sizeof(int));

	/* Generate random data */
	if (random) {
		generate_matrix_random(m, n, Af, Alda);
		generate_vector_random(n, xf);
		generate_vector_random(m, bf);
	}
	else {
		generate_matrix(m, n, Af, Alda, init, incr);
		generate_vector(n, xf, init, incr);
		generate_vector(m, bf, 0.0, 0.0);
	}
	matrix_vector_product(m, n, Af, Alda, xf, bf);

	/* Print data */
	if (myId == ROOT) {
		printf("   Problem dimension, m = %d, n = %d\n", m, n);
        printf("   Strategy: Row pivoting\n");
		printf("   Processes = %d, Threads per process = %s\n", size, getenv("OMP_NUM_THREADS"));
		if (visual == 1) {
			print_matrix("Ai", m, n, Af, Alda);
			print_vector("xi", n, x);
			print_vector("bi", m, bf);
		}

		printf("-->Solve problem\n");
	}

	//Cálculo para el reparto
	sendFromA = (int*)malloc(size * sizeof(int));
	sizeToSendA = (int*)malloc(size * sizeof(int));

	bloque = m / size;
	n_bloque = m % size;

	//Calculo de como repartir las filas de la matriz A y los elementos del vector B 
	for (i = 0; i < size; i++) {
		sendFromA[i] = (i == 0) ? 0 : sendFromA[i - 1] + sizeToSendA[i - 1];
		sizeToSendA[i] = (i < n_bloque) ? (bloque + 1) : bloque;
	}

	//Creacion de fila para reparto
	MPI_Datatype rowType;
	MPI_Type_contiguous(n, MPI_DOUBLE, &rowType);
	MPI_Type_commit(&rowType);

	nreps = 0;
	time = 0.0;
	vtemp = (double*)malloc(m * sizeof(double));
	while ((info == 0) && (time < tmin)) {
		if (myId == ROOT) {
			copy_matrix(m, n, Af, Alda, A, Alda);
			copy_vector(m, bf, b);
		}
		generate_vector(n, x, 0.0, 0.0);
		generate_ivector(m, vPiv, 1, 1);

		/* LU factorization */
		MPI_Barrier(MPI_COMM_WORLD);
		t1 = MPI_Wtime();
		dim = m < n ? m : n;

		rowsToReceive = sizeToSendA[myId];
		startFrom = sendFromA[myId];
		
		//Recibo los datos en las estructuras de datos originales
		//Por ejemplo con 2 procesos, P1 recibira la segunda parte de la matriz en las columnas correspondientes de la estructura original
		//Se que no es eficiente a nivel de memoria, 
        //pero asi evito tener que lidiar con transformaciones en los indices y vivir unos añitos más :)

		//Reparto de las filas de la matriz correspondientes a cada proceso
		MPI_Scatterv(A, sizeToSendA, sendFromA, rowType, myId == ROOT ? MPI_IN_PLACE : &Aref(startFrom + 1, 1), rowsToReceive, rowType, ROOT, MPI_COMM_WORLD);
		//Reparto de los elementos del vector correspondientes a cada proceso
		MPI_Scatterv(b, sizeToSendA, sendFromA, MPI_DOUBLE, myId == ROOT ? MPI_IN_PLACE : &bref(startFrom + 1), rowsToReceive, MPI_DOUBLE, ROOT, MPI_COMM_WORLD);

		processWorking = 0;
        //He decidido emplear solo una region paralela para que los threads solo se lancen una vez
		#pragma omp parallel private(k)
		{
			for (k = 1; k <= dim; k++) {
				//Se determina a que proceso corresponde la fila que se esta tratando
				#pragma omp single
				{
					if (processWorking != size - 1 && k > sendFromA[processWorking + 1]) {
						processWorking++;
					}
				}

				//El proceso al que corresponde la fila, calcula el máximo
				piv = -1; iPiv = -1;
				double piv_local = dabs(Aref(k, k));
				int iPiv_local = k;
				double currVal;
				if (processWorking == myId) {
					#pragma omp for
					for (i = k + 1; i <= n; i++) {
						currVal = dabs(Aref(k, i));
						if (piv_local < currVal) {
							piv_local = currVal;
							iPiv_local = i;
						}
					}

					#pragma omp critical
					{
						if (piv_local > piv) {
							piv = piv_local;
							iPiv = iPiv_local;
						}
					}
					
					#pragma omp barrier //Barrier para que todos tengan el iPiv correcto

					#pragma omp master
					{
						piv = Aref(k, iPiv);
					}
				}

				//Se distribuye el indice (columna) donde está el máximo
				#pragma omp barrier
				#pragma omp master
				{
					MPI_Bcast(&iPiv, 1, MPI_INT, processWorking, MPI_COMM_WORLD);
				}
				#pragma omp barrier

				//Todos los procesos realizan el swap en sus correspondientes filas
				if (iPiv != k) {
					#pragma omp for
					for (i = startFrom + 1; i <= startFrom + rowsToReceive; i++) {
						vtemp[i - 1] = Aref(i, k);
						Aref(i, k) = Aref(i, iPiv);
						Aref(i, iPiv) = vtemp[i - 1];
					}

					#pragma omp master //No es necesaria barrera
					{
						int ptmp = dref(k);
						dref(k) = dref(iPiv);
						dref(iPiv) = ptmp;
					}
				}


				if (processWorking == myId) {
					#pragma omp master
					{
						bref(k) /= piv;
					}

					#pragma omp for
					for (i = k; i <= n; i++) {
						Aref(k, i) /= piv;
					}
				}

				//Se distribuye la fila y el elemento del vector
				#pragma omp barrier
				#pragma omp master
				{
					//MPI_Bcast(&Aref(k, 1), 1, rowType, processWorking, MPI_COMM_WORLD);
					//MPI_Bcast(&bref(k), 1, MPI_DOUBLE, processWorking, MPI_COMM_WORLD);
					//Enviamos Aref(k,1) y bref(k) a los procesos que lo van a necesitar (id > k)
                    //Tambien se podría hacer Bcast, pero no todos los procesos lo necesitan
                    if (processWorking == myId){
                        for (i = myId + 1; i < size; i++) {
                            MPI_Send(&Aref(k, 1), 1, rowType, i, TAG + 1, MPI_COMM_WORLD);
                            MPI_Send(&bref(k), 1, MPI_DOUBLE, i, TAG, MPI_COMM_WORLD);
                        }
                    } else if (processWorking < myId) {
                        MPI_Recv(&Aref(k, 1), 1, rowType, processWorking, TAG + 1, MPI_COMM_WORLD, &st);
                        MPI_Recv(&bref(k), 1, MPI_DOUBLE, processWorking, TAG, MPI_COMM_WORLD, &st);
                    }
				}
				#pragma omp barrier

				//Cada proceso realiza el calculo en sus filas (limite inferior --> if, limite superior --> cond. primer for)
				#pragma omp for private(j) schedule(dynamic)
				for (i = k + 1; i <= startFrom + rowsToReceive; i++) {
					if (i > startFrom) {
						for (j = k + 1; j <= n; j++) {
							Aref(i, j) -= Aref(i, k) * Aref(k, j);
						}
						bref(i) -= Aref(i, k) * bref(k);
					}
				}
			}

			#pragma omp barrier
			#pragma omp master //El tiempo solo toma un thread del proceso
			{
				MPI_Barrier(MPI_COMM_WORLD);
				t2 = MPI_Wtime();
				timeLU += (t2 > t1 ? t2 - t1 : 0.0);
			}


			#pragma omp barrier
			#pragma omp master
			{
				MPI_Barrier(MPI_COMM_WORLD);
				t1 = MPI_Wtime();
			}

			// Backward substitution
			processWorkingA = size - 1;
			for (k = dim; k > 0; k--) {
				//Se determina que proceso tiene  el elemento k del vector
				//y se comunica bref(k)
				int processWorkingB = 0;
				#pragma omp barrier
				#pragma omp master
				{
					if (processWorkingA != 0 && k == sendFromA[processWorkingA]) {
						processWorkingA--;
					}

					if (processWorkingA == myId){
						for (i = myId - 1; i >= 0; i--) {
		                   MPI_Send(&bref(k), 1, MPI_DOUBLE, i, TAG, MPI_COMM_WORLD);
						}
					} else if (processWorkingA > myId) {
						MPI_Recv(&bref(k), 1, MPI_DOUBLE, processWorkingA, TAG, MPI_COMM_WORLD, &st);
					}
				}
				#pragma omp barrier
				
				#pragma omp for private(j) schedule(dynamic)
				for (i = 1; i <= k - 1; i++) {					
					if (processWorkingB != size - 1 && i > sendFromA[processWorkingB + 1]) {
          				processWorkingB++;
        			}

					if (processWorkingB == myId) {
						bref(i) -= bref(k) * Aref(i, k);
					}
				}
			}

			//Root recoge los elementos del vector para realizar la permutación
			#pragma omp barrier
			#pragma omp master
			{
				MPI_Gatherv(myId == ROOT ? MPI_IN_PLACE : &bref(startFrom + 1), rowsToReceive, MPI_DOUBLE, b, sizeToSendA, sendFromA, MPI_DOUBLE, ROOT, MPI_COMM_WORLD);
			}
			#pragma omp barrier

			// Remove permutation
			#pragma omp for
			for (i = 1; i <= n; i++) {
				xref(dref(i)) = bref(i);
			}
		}

		//Reparto de las soluciones
		MPI_Scatterv(x, sizeToSendA, sendFromA, MPI_DOUBLE, myId == ROOT ? MPI_IN_PLACE : &xref(startFrom + 1), rowsToReceive, MPI_DOUBLE, ROOT, MPI_COMM_WORLD);

		MPI_Barrier(MPI_COMM_WORLD);
		t2 = MPI_Wtime();

		timeTr += (t2 > t1 ? t2 - t1 : 0.0);

		time = timeLU + timeTr;
		nreps++;
	}

	if (info != 0) {
		printf("Error in problem solution\n");
		exit(-1);
	}

	timeLU /= nreps;
	timeTr /= nreps;

	/* Print results */
	if (myId == ROOT) {
		if (visual == 1) {
			print_matrix("Af", m, n, A, Alda);
			print_vector("xf", n, x);
			print_vector("bf", m, b);
		}

		printf("-->Results\n");
		printf("   Residual     = %12.6e\n", i, compute_error(m, x, xf));
		printf("   Time LU      = %12.6e seg.\n", timeLU);
		flops = ((double)n) * n * (m - n / 3.0);
		GFLOPsLU = flops / (1.0e+9 * timeLU);
		printf("   GFLOPs LU    = %12.6e     \n", GFLOPsLU);

		printf("   Time Tr      = %12.6e seg.\n", timeTr);
		flops = ((double)n) * n;
		GFLOPsTr = flops / (1.0e+9 * timeTr);
		printf("   GFLOPs Tr    = %12.6e     \n", GFLOPsTr);
		printf("End of program...\n");
		printf("----------------------------------------------------------\n");
	}

	/* Free data */
	free(Af); free(A);
	free(xf); free(x);
	free(bf); free(b);
	free(vPiv);
	free(sendFromA);
	free(sizeToSendA);
	free(vtemp);

	MPI_Finalize();
	return 0;
}
