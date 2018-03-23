#include "LPSolver.hpp"

#include <chrono>
#include <iostream>

#define PSD_EIGEN_TOL 0.00001
#define MAX_TRIES_ROUNDING 5

//Construct solver
LPSolver::LPSolver(Problem* p) {
	problem = p;
	nQP = p->nQP;
	nLP = nQP*(nQP-1)/2;
	
	//Establish an initial lower bound through guessing the naive all-one vector
	bestSol = VectorXd(nQP);
	bestSol.setOnes();
	lowerBound = p->score(bestSol);
	
	//Establish an initial upper bound by summing abs of each coefficient
	upperBound = p->constantTerm;
	for(uint32_t i=0;i<nQP;i++){
		for(uint32_t j=i+1;j<nQP;j++){
			upperBound += fabs(p->coeffs(i,j));
		}
	}
	
	//Create GLPK instance
	lp = glp_create_prob();
	
	glp_set_prob_name(lp, "CLQO"); //Name the problem
	glp_set_obj_dir(lp, GLP_MAX);  //We maximize
	
	//each column corresponds to a variable we (linearly) optimize over
	glp_add_cols(lp, nLP);
	for(uint32_t i=1;i<=nLP;i++){
		//Each variable has DB (double bound) to [-1,+1]
		glp_set_col_bnds(lp, i, GLP_DB, -1., 1.);
		std::pair<int,int> xy = getQPVars(i);
		
		//Set objective weight
		glp_set_obj_coef(lp, i, p->coeffs(xy.second, xy.first));
	}
	
	active_clauses = std::vector<constraint>();
}

//Given a variable vi and vj, get the index of vij
uint32_t LPSolver::getLPVar(uint32_t x, uint32_t y){
	if(y == x) throw std::runtime_error(std::string("Bad LP: ")+std::to_string(x)+", "+std::to_string(y));
	if(y > x) return getLPVar(y, x);
	if(y >= nQP || x >= nQP) throw std::runtime_error(std::string("Bad LP: ")+std::to_string(x)+", "+std::to_string(y)+", "+std::to_string(nQP));
	return 1 + y + x*(x-1)/2;
}

//given vij, find i and j
std::pair<uint32_t,uint32_t> LPSolver::getQPVars(uint32_t v){
	if(v > nLP) throw std::runtime_error(std::string("Bad vQP: ")+std::to_string(v)+", "+std::to_string(nLP));
	
	uint32_t x = floor(sqrt(2*v)+1./2);
	uint32_t y = v - 1 - x*(x-1)/2;
	
	if(y >= x) throw std::runtime_error(std::string("Bad QP: ")+std::to_string(x)+", "+std::to_string(y)+", "+std::to_string(v));
	
	return std::pair<uint32_t,uint32_t>(x,y);
}

//Given an assignment, find a minimal (but non necess. minimum) set of
//rows/columns that has a constraint that's it violating.
//If it's actually positive semidefinite, and there are not constraints
//that it's violating, return the empty list. (Congrats! This means you
//found the global optimum!) 
std::vector<uint32_t>& LPSolver::nonPSDcore(){
	//Two phases: one where we add rows hoping to make it not PSD,
	//and then later we remove rows to make it minimal.
	
	//Rows we're currently taking
	std::vector<uint32_t>& core = *new std::vector<uint32_t>();
	//Rows we have not taken
	std::vector<uint32_t> notInCore;
	for(uint32_t i=0;i<nQP;i++) notInCore.push_back(i);
	
	//put the rows in a random order
	std::random_shuffle(notInCore.begin(), notInCore.end());
	
	do{
		uint32_t newRow = notInCore.back();
		notInCore.pop_back();
		core.push_back(newRow);
		
		MatrixXd& subMat = getSubmatrix(core);
		//std::cout << subMat << std::endl;
		
		Eigen::SelfAdjointEigenSolver<MatrixXd> eig(subMat);
		auto evals = eig.eigenvalues();
		bool isPSD = true;
		for(uint32_t i=0; i<evals.size(); i++){
			if(evals[i] < -PSD_EIGEN_TOL){
				isPSD = false;
				break;
			}
		}
		delete &subMat;
		
		if(isPSD){
			if(notInCore.size() > 0)
				continue; //add more
			else {
				core.clear();
				return core; //nothing nonPSD, return empty vector
			}
		} else {
			//we found a non-PSD chunk!
			break;
		}
		
	}while(true);
	
	/*printf("nonPSD core found in vars \n");
	for(uint32_t i=0;i<core.size();i++)
		printf("%d, ", core[i]);
	printf("\n");*/
	
	uint32_t largeMatSize = core.size();
	for(uint32_t iter=0; iter<largeMatSize; iter++){
		uint32_t removedRow = core[0];
		//std::cout << "Pared down " << removedRow << std::endl;
		core.erase(core.begin());
		
		MatrixXd& subMat = getSubmatrix(core);
		//std::cout << subMat << std::endl;
		
		Eigen::SelfAdjointEigenSolver<MatrixXd> eig(subMat);
		auto evals = eig.eigenvalues();
		bool isPSD = true;
		for(uint32_t i=0; i<evals.size(); i++){
			if(evals[i] < -PSD_EIGEN_TOL){
				isPSD = false;
				break;
			}
		}
		delete &subMat;
		
		if(isPSD) //the row we removed was necessary for non-PSD-ness, add it back in
			core.push_back(removedRow);
		//else great, we didn't need it. do nothing and repeat.
	}
	
	/*printf("nonPSD core found in vars \n");
	for(uint32_t i=0;i<core.size();i++)
		printf("%d, ", core[i]);
	printf("\n");*/
	return core;
}

MatrixXd& LPSolver::getSubmatrix(std::vector<uint32_t> rows){
	
	MatrixXd& result = *new MatrixXd(rows.size(), rows.size());
	for(uint32_t i=0;i<rows.size();i++){
		result(i,i) = 1;
		
		for(uint32_t j=i+1;j<rows.size();j++)
			result(j,i) = result(i,j) = currSol[getLPVar(rows[i],rows[j])-1];
	}
	
	return result;
}

MatrixXd& LPSolver::getMatrix(){
	MatrixXd& result = *new MatrixXd(nQP, nQP);
	for(uint32_t i=0;i<nQP;i++){
		result(i,i) = 1;
		
		for(uint32_t j=i+1;j<nQP;j++)
			result(j,i) = result(i,j) = currSol[getLPVar(i,j)-1];
	}
	
	return result;
}

void LPSolver::roundToSol(){//TODO write the solution to the problem
	
	//TODO call to an actual SDP solver
	
	MatrixXd& solMat = getMatrix();
	
	std::cout << "The matrix Sol:" << std::endl << solMat << std::endl;
	
	//for now --
	//Reduce 'magnitude' slightly to make PSD
	Eigen::SelfAdjointEigenSolver<MatrixXd> eig(solMat);
	auto evals = eig.eigenvalues();
	double minEigenvalue = evals[0] - 0.00001;
	
	solMat *= -1. / (-1 + minEigenvalue);
	solMat += MatrixXd::Identity(nQP, nQP) * (minEigenvalue / (-1 + minEigenvalue));
	
	std::cout << "SDP-ified Sol:" << std::endl;
	
	std::cout << solMat << std::endl;
	Eigen::LLT<MatrixXd> lltOfA(solMat); // compute the Cholesky decomposition of A
	MatrixXd L = lltOfA.matrixL(); // retrieve factor L  in the decomposition
	// The previous two lines can also be written as "L = A.llt().matrixL()"
	std::cout << "The Cholesky factor L is" << std::endl << L << std::endl;
	
	delete &solMat;
	
	for(int tries=0; tries<MAX_TRIES_ROUNDING; tries++){
		//generate random dot vector
		VectorXd v(nQP);
		std::default_random_engine generator(std::chrono::high_resolution_clock::now().time_since_epoch().count());
		std::normal_distribution<double> distribution(0.0,1.0);
		for(uint32_t i=0;i<nQP;i++){
			v(i) = distribution(generator);
			if(i==0) v(i) = fabs(v(i)); //normalize solution to start with "1"
		}
		
		VectorXd resultVec = L * v;
		//std::cout << "Dot product vector" << resultVec.transpose() << std::endl;
		for(uint32_t i=0;i<nQP;i++)
			resultVec(i) = std::copysign(1.0,resultVec(i));
		
		/*std::cout << "Final assignment: " << std::endl;
		for(uint32_t i=0;i<nQP;i++)
			printf("%f, ", resultVec(i));
		puts("");*/
		
		printf("Score = %f\n", problem->score(resultVec));
	}
}

void LPSolver::solve(){
	//TODO hoo-wee!
}