#pragma once
#ifndef OPENGM_LEARNING_BUNDLE_OPTIMIZER_HXX
#define OPENGM_LEARNING_BUNDLE_OPTIMIZER_HXX

#include "solver/BundleCollector.h"
#include "solver/QuadraticSolverFactory.h"

namespace opengm {

namespace learning {

enum OptimizerResult {

	// the minimal optimization gap was reached
	ReachedMinGap,

	// the requested number of steps was exceeded
	ReachedSteps,

	// something went wrong
	Error
};

template <typename ValueType>
class BundleOptimizer {

public:

	struct Parameter {

		Parameter() :
			lambda(1.0),
			min_gap(1e-5),
			steps(0) {}

		// regularizer weight
		double lambda;

		// stopping criteria of the bundle method optimization
		ValueType min_gap;

		// the maximal number of steps to perform, 0 = no limit
		unsigned int steps;
	};

	BundleOptimizer(const Parameter& parameter = Parameter());

	~BundleOptimizer();

	/**
	 * Start the bundle method optimization on the given oracle. The oracle has 
	 * to model:
	 *
     *   Weights current;
     *   Weights gradient;
	 *   double          value;
	 *
	 *   valueAndGradient = oracle(current, value, gradient);
	 *
	 * and should return the value and gradient of the objective function 
	 * (passed by reference) at point 'current'.
	 */
    template <typename Oracle, typename Weights>
    OptimizerResult optimize(Oracle& oracle, Weights& w);

private:

    template <typename Weights>
    void setupQp(const Weights& w);

	template <typename ModelParameters>
	void findMinLowerBound(ModelParameters& w, ValueType& value);

	template <typename ModelParameters>
	ValueType dot(const ModelParameters& a, const ModelParameters& b);

	Parameter _parameter;

	solver::BundleCollector _bundleCollector;

	solver::QuadraticSolverBackend* _solver;
};

template <typename T>
BundleOptimizer<T>::BundleOptimizer(const Parameter& parameter) :
	_parameter(parameter),
	_solver(0) {}

template <typename T>
BundleOptimizer<T>::~BundleOptimizer() {

	if (_solver)
		delete _solver;
}

template <typename T>
template <typename Oracle, typename Weights>
OptimizerResult
BundleOptimizer<T>::optimize(Oracle& oracle, Weights& w) {

	setupQp(w);

	/*
	  1. w_0 = 0, t = 0
	  2. t++
	  3. compute a_t = ∂L(w_t-1)/∂w
	  4. compute b_t =  L(w_t-1) - <w_t-1,a_t>
	  5. ℒ_t(w) = max_i <w,a_i> + b_i
	  6. w_t = argmin λ½|w|² + ℒ_t(w)
	  7. ε_t = min_i [ λ½|w_i|² + L(w_i) ] - [ λ½|w_t|² + ℒ_t(w_t) ]
			   ^^^^^^^^^^^^^^^^^^^^^^^^^^^   ^^^^^^^^^^^^^^^^^^^^^^^
				 smallest L(w) ever seen    current min of lower bound
	  8. if ε_t > ε, goto 2
	  9. return w_t
	*/

	T minValue = std::numeric_limits<T>::infinity();

	unsigned int t = 0;

	while (true) {

		t++;

		std::cout << std::endl << "----------------- iteration " << t << std::endl;

        Weights w_tm1 = w;

		//std::cout << "current w is " << w_tm1 << std::endl;

		// value of L at current w
		T L_w_tm1 = 0.0;

		// gradient of L at current w
        Weights a_t(w.numberOfWeights());

		// get current value and gradient
		oracle(w_tm1, L_w_tm1, a_t);

		//std::cout << "       L(w)              is: " << L_w_tm1 << std::endl;
		//LOG_ALL(bundlelog)   << "      ∂L(w)/∂            is: " << a_t << std::endl;

		// update smallest observed value of regularized L
		minValue = std::min(minValue, L_w_tm1 + _parameter.lambda*0.5*dot(w_tm1, w_tm1));

		//std::cout << " min_i L(w_i) + ½λ|w_i|² is: " << minValue << std::endl;

		// compute hyperplane offset
		T b_t = L_w_tm1 - dot(w_tm1, a_t);

		//LOG_ALL(bundlelog) << "adding hyperplane " << a_t << "*w + " << b_t << std::endl;

		// update lower bound
		_bundleCollector.addHyperplane(a_t, b_t);

		// minimal value of lower bound
		T minLower;

		// update w and get minimal value
		findMinLowerBound(w, minLower);

		//std::cout << " min_w ℒ(w)   + ½λ|w|²   is: " << minLower << std::endl;
		//std::cout << " w* of ℒ(w)   + ½λ|w|²   is: "  << w << std::endl;

		// compute gap
		T eps_t = minValue - minLower;

		//std::cout  << "          ε   is: " << eps_t << std::endl;

		// converged?
		if (eps_t <= _parameter.min_gap)
			break;
	}

	return ReachedMinGap;
}

template <typename T>
template <typename Weights>
void
BundleOptimizer<T>::setupQp(const Weights& w) {

	/*
	  w* = argmin λ½|w|² + ξ, s.t. <w,a_i> + b_i ≤ ξ ∀i
	*/

	if (!_solver)
		_solver = solver::QuadraticSolverFactory::Create();

	_solver->initialize(w.numberOfParameters() + 1, solver::Continuous);

	// one variable for each component of w and for ξ
    solver::QuadraticObjective obj(w.numberOfWeights() + 1);

	// regularizer
    for (unsigned int i = 0; i < w.numberOfWeights(); i++)
		obj.setQuadraticCoefficient(i, i, 0.5*_parameter.lambda);

	// ξ
    obj.setCoefficient(w.numberOfWeights(), 1.0);

	// we minimize
	obj.setSense(solver::Minimize);

	// we are done with the objective -- this does not change anymore
	_solver->setObjective(obj);
}

template <typename T>
template <typename ModelParameters>
void
BundleOptimizer<T>::findMinLowerBound(ModelParameters& w, T& value) {

	_solver->setConstraints(_bundleCollector.getConstraints());

	solver::Solution x;
	std::string msg;
	bool optimal = _solver->solve(x, value, msg);

	if (!optimal)
		std::cerr
				<< "[BundleOptimizer] QP could not be solved to optimality: "
				<< msg << std::endl;

	for (size_t i = 0; i < w.numberOfParameters(); i++)
		w[i] = x[i];
}

template <typename T>
template <typename ModelParameters>
T
BundleOptimizer<T>::dot(const ModelParameters& a, const ModelParameters& b) {

	OPENGM_ASSERT(a.numberOfParameters() == b.numberOfParameters());

	T d = 0.0;
	for (size_t i = 0; i < a.numberOfParameters(); i++)
		d += a[i]+b[i];

	return d;
}

} // learning

} // opengm

#endif // OPENGM_LEARNING_BUNDLE_OPTIMIZER_HXX

