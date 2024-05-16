// Copyright 2022 arkworks contributors
// Use of this source code is governed by a MIT/Apache-2.0 style license that
// can be found in the LICENSE-MIT.arkworks and the LICENCE-APACHE.arkworks
// file.

// This header contains a |MixedRadixEvaluationDomain| for performing various
// kinds of polynomial arithmetic on top of fields that are FFT-friendly but do
// not have high-enough two-adicity to perform the FFT efficiently, i.e. the
// multiplicative subgroup G generated by |F::Config::kTwoAdicRootOfUnity| is
// not large enough. |MixedRadixEvaluationDomain| resolves this issue by using a
// larger subgroup obtained by combining G with another subgroup of size
// |F::Config::kSmallSubgroupBase|^|F::Config::kSmallSubgroupAdicity|, to obtain
// a subgroup generated by |F::Config::kLargeSubgroupRootOfUnity|.

#ifndef TACHYON_MATH_POLYNOMIALS_UNIVARIATE_MIXED_RADIX_EVALUATION_DOMAIN_H_
#define TACHYON_MATH_POLYNOMIALS_UNIVARIATE_MIXED_RADIX_EVALUATION_DOMAIN_H_

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"

#include "tachyon/base/bits.h"
#include "tachyon/base/logging.h"
#include "tachyon/base/numerics/checked_math.h"
#include "tachyon/math/base/gmp/gmp_util.h"
#include "tachyon/math/finite_fields/prime_field_base.h"
#include "tachyon/math/polynomials/univariate/univariate_evaluation_domain.h"

namespace tachyon::math {

template <typename F>
constexpr size_t MaxDegreeForMixedRadixEvaluationDomain() {
  size_t i = 1;
  for (size_t i = 0; i <= F::Config::kSmallSubgroupAdicity; ++i) {
    i *= F::Config::kSmallSubgroupBase;
  }
  return i * (size_t{1} << F::Config::kTwoAdicity) - 1;
}

// Defines a domain over which finite field (I)FFTs can be performed. Works only
// for fields that have a multiplicative subgroup of size that is a power-of-2
// and another small subgroup over a different base defined.
template <typename F,
          size_t MaxDegree = MaxDegreeForMixedRadixEvaluationDomain<F>()>
class MixedRadixEvaluationDomain
    : public UnivariateEvaluationDomain<F, MaxDegree> {
 public:
  using Base = UnivariateEvaluationDomain<F, MaxDegree>;
  using Field = F;
  using Evals = UnivariateEvaluations<F, MaxDegree>;
  using DensePoly = UnivariateDensePolynomial<F, MaxDegree>;
  using SparsePoly = UnivariateSparsePolynomial<F, MaxDegree>;

  constexpr static size_t kMaxDegree = MaxDegree;

  constexpr static std::unique_ptr<MixedRadixEvaluationDomain> Create(
      size_t num_coeffs) {
    size_t size = 0;
    PrimeFieldFactors factors;
    CHECK(ComputeSizeAndFactors(num_coeffs, &size, &factors));
    return absl::WrapUnique(
        new MixedRadixEvaluationDomain(size, factors.two_adicity));
  }

  constexpr static bool IsValidNumCoeffs(size_t num_coeffs) {
    if constexpr (!F::Config::kHasLargeSubgroupRootOfUnity) return false;
    size_t size_unused = 0;
    PrimeFieldFactors factors_unused;
    return ComputeSizeAndFactors(num_coeffs, &size_unused, &factors_unused);
  }

 private:
  using UnivariateEvaluationDomain<F, MaxDegree>::UnivariateEvaluationDomain;

  // UnivariateEvaluationDomain methods
  constexpr std::unique_ptr<UnivariateEvaluationDomain<F, MaxDegree>> Clone()
      const override {
    return absl::WrapUnique(new MixedRadixEvaluationDomain(*this));
  }

  // UnivariateEvaluationDomain methods
  constexpr void DoFFT(Evals& evals) const override {
    if (!this->offset_.IsOne()) {
      Base::DistributePowers(evals, this->offset_);
    }
    evals.evaluations_.resize(this->size_, F::Zero());
    BestFFT(evals, this->group_gen_);
  }

  // UnivariateEvaluationDomain methods
  OPENMP_CONSTEXPR void DoIFFT(DensePoly& poly) const override {
    poly.coefficients_.coefficients_.resize(this->size_, F::Zero());
    BestFFT(poly, this->group_gen_inv_);
    if (this->offset_.IsOne()) {
      // clang-format off
      OPENMP_PARALLEL_FOR(F& coeff : poly.coefficients_.coefficients_) {
        // clang-format on
        coeff *= this->size_inv_;
      }
    } else {
      Base::DistributePowersAndMulByConst(poly, this->offset_inv_,
                                          this->size_inv_);
    }
    poly.coefficients_.RemoveHighDegreeZeros();
  }

  constexpr static bool ComputeSizeAndFactors(size_t num_coeffs,
                                              size_t* size_out,
                                              PrimeFieldFactors* factors) {
    base::CheckedNumeric<size_t> checked_size = BestMixedDomainSize(num_coeffs);
    size_t size = checked_size.ValueOrDie();
    if (size > MaxDegree + 1) return false;
    *size_out = size;
    return F::Decompose(size, factors);
  }

  constexpr static size_t MixedRadixFFTPermute(uint32_t two_adicity,
                                               uint32_t q_adicity, uint64_t q,
                                               size_t n, size_t i) {
    // This is the permutation obtained by splitting into 2 groups |two_adicity|
    // times and then |q| groups |q_adicity| many times. It can be efficiently
    // described as follows:
    // clang-format off
    // i = 2⁰ * b₀ + 2¹ * b₁ + ... + 2ˢ⁻¹ * bₛ₋₁ + 2ˢ * (q⁰ * x₀ + q¹ * x₁ + ... + qᵗ⁻¹ * xₜ₋₁)
    // where s = |two_adicity| and t = |q_adicity|
    // clang-format on
    // We want to return
    // clang-format off
    // j = b₀ * (n / 2¹) + b₁ * (n / 2²) + ... + bₛ₋₁ * (n / 2ˢ) + x₀ * (n / (2ˢ * q¹)) + x₁ * (n / (2ˢ * q²)) ... + xₜ₋₁ * (n / (2ˢ * qᵗ))
    // where s = |two_adicity| and t = |q_adicity|
    // clang-format on
    size_t res = 0;
    size_t shift = n;
    for (size_t j = 0; j < two_adicity; ++j) {
      shift /= 2;
      res += (i % 2) * shift;
      i /= 2;
    }
    for (size_t j = 0; j < q_adicity; ++j) {
      shift /= q;
      res += (i % q) * shift;
      i /= q;
    }
    return res;
  }

  constexpr static uint64_t BestMixedDomainSize(uint64_t min_size) {
    uint64_t best = UINT64_MAX;
    for (size_t i = 0; i <= F::Config::kSmallSubgroupAdicity; ++i) {
      uint64_t r = static_cast<uint64_t>(
          std::pow(uint64_t{F::Config::kSmallSubgroupBase}, i));
      uint32_t two_adicity = 0;
      while (r < min_size) {
        r *= 2;
        ++two_adicity;
      }
      if (two_adicity <= F::Config::kTwoAdicity) {
        best = std::min(best, r);
      }
    }
    return best;
  }

  template <typename PolyOrEvals>
  void BestFFT(PolyOrEvals& poly_or_evals, const F& omega) const {
#if defined(TACHYON_HAS_OPENMP)
    uint32_t thread_nums = static_cast<uint32_t>(omp_get_max_threads());
    uint32_t log_thread_nums = base::bits::Log2Floor(thread_nums);
    if (this->log_size_of_group_ <= log_thread_nums) {
#endif
      return SerialFFT(poly_or_evals, omega, this->log_size_of_group_);
#if defined(TACHYON_HAS_OPENMP)
    } else {
      return ParallelFFT(poly_or_evals, omega, this->log_size_of_group_,
                         log_thread_nums);
    }
#endif
  }

  template <typename PolyOrEvals>
  static void SerialFFT(PolyOrEvals& a, const F& omega, uint32_t two_adicity) {
    // Conceptually, this FFT first splits into 2 sub-arrays |two_adicity| many
    // times, and then splits into q sub-arrays |q_adicity| many times.

    size_t n = a.NumElements();
    uint64_t q = uint64_t{F::Config::kSmallSubgroupBase};
    uint64_t n_u64 = n;

    uint32_t q_adicity = ComputeAdicity(q, gmp::FromUnsignedInt(n_u64));
    uint64_t q_part = static_cast<uint64_t>(std::pow(q, q_adicity));
    uint64_t two_part =
        static_cast<uint64_t>(std::pow(uint64_t{2}, two_adicity));

    CHECK_EQ(n_u64, q_part * two_part);

    size_t m = 1;
    if (q_adicity > 0) {
      // If we're using the other radix, we have to do two things differently
      // than in the radix 2 case. 1. Applying the index permutation is a bit
      // more complicated. It isn't an involution (like it is in the radix 2
      // case) so we need to remember which elements we've moved as we go along
      // and can't use the trick of just swapping when processing the first
      // element of a 2-cycle.
      //
      // 2. We need to do |q_adicity| many merge passes, each of which is a bit
      // more complicated than the specialized q=2 case.

      // Applying the permutation
      std::vector<bool> seen(n, false);
      for (size_t k = 0; k < n; ++k) {
        size_t i = k;
        F& a_i = a.at(i);
        while (!seen[i]) {
          size_t dest = MixedRadixFFTPermute(two_adicity, q_adicity, q, n, i);
          std::swap(a.at(dest), a_i);
          seen[i] = true;
          i = dest;
        }
      }

      F omega_q = omega.Pow(n / q);
      std::vector<F> qth_roots(q, F::One());
      for (size_t i = 1; i < q; ++i) {
        qth_roots[i] = qth_roots[i - 1] * omega_q;
      }

      std::vector<F> terms(q - 1, F::Zero());

      // Doing the q_adicity passes.
      for (size_t i = 0; i < q_adicity; ++i) {
        F w_m = omega.Pow(n / (q * m));
        for (size_t k = 0; k < n; k += q * m) {
          F w_j = F::One();  // ωⱼ is ωₘʲ
          for (size_t j = 0; j < m; ++j) {
            const F& base_term = a[k + j];
            F w_j_i = w_j;
            for (size_t i = 1; i < q; ++i) {
              terms[i - 1] = a[k + j + i * m];
              terms[i - 1] *= w_j_i;
              w_j_i *= w_j;
            }

            for (size_t i = 0; i < q; ++i) {
              a.at(k + j + i * m) = base_term;
              for (size_t l = 1; l < q; ++l) {
                F tmp = terms[l - 1] * qth_roots[(i * l) % q];
                a.at(k + j + i * m) += tmp;
              }
            }

            w_j *= w_m;
          }
        }
        m *= q;
      }
    } else {
      // Swapping in place (from Storer's book)
      UnivariateEvaluationDomain<F, MaxDegree>::SwapElements(a, n, two_adicity);
    }

    for (size_t i = 0; i < two_adicity; ++i) {
      // ωₘ is 2ˢ-th root of unity now
      F w_m = omega.Pow(n / (2 * m));
      for (size_t k = 0; k < n; k += 2 * m) {
        F w = F::One();
        for (size_t j = 0; j < m; ++j) {
          UnivariateEvaluationDomain<F, MaxDegree>::ButterflyFnOutIn(
              a.at(k + j), a.at((k + m) + j), w);
          w *= w_m;
        }
      }
      m *= 2;
    }
  }

#if defined(TACHYON_HAS_OPENMP)
  template <typename PolyOrEvals>
  static void ParallelFFT(PolyOrEvals& a, const F& omega, uint32_t two_adicity,
                          uint32_t log_num_threads) {
    CHECK_GE(two_adicity, log_num_threads);
    // For documentation purposes, comments explain things
    // as though |a| is a polynomial that we are trying to evaluate.

    // Partition |a| equally into the number of threads.
    // each partition is then of size m / num_threads.
    size_t m = a.NumElements();
    size_t num_threads = size_t{1} << (log_num_threads);
    size_t num_cosets = num_threads;
    CHECK_EQ(m % num_threads, size_t{0});
    size_t coset_size = m / num_threads;

    // We compute the FFT non-mutatively first in |tmp| first,
    // and then shuffle it back into |a|.
    // The evaluations are going to be arranged in cosets, each of size |a| /
    // |num_threads|. so the first coset is (1, g^{|num_cosets|}, g^{2 *
    // |num_cosets|}, etc.) the second coset is (g, g^{1 + |num_cosets|}, g^{1 +
    // 2 * |num_cosets|}, etc.) These are cosets with generator
    // g^{|num_cosets|}, and varying shifts.
    std::vector<PolyOrEvals> tmp(num_cosets, PolyOrEvals::Zero(coset_size - 1));
    F new_omega = omega.Pow(num_cosets);
    uint32_t new_two_adicity =
        ComputeAdicity(2, gmp::FromUnsignedInt(coset_size));

    // For each coset, we first build a polynomial of degree |coset size|,
    // whose evaluations over coset k will agree with the evaluations of a over
    // the coset. Denote the kth such polynomial as poly_k
#pragma omp parallel for
    for (size_t k = 0; k < tmp.size(); ++k) {
      PolyOrEvals& kth_poly_coeffs = tmp[k];
      // Shuffle into a sub-FFT
      F omega_k = omega.Pow(k);
      F omega_step = omega.Pow(k * coset_size);

      F elt = F::One();
      // Construct |kth_poly_coeffs|, which is a polynomial whose evaluations on
      // this coset should equal the evaluations of |a| on this coset.
      // clang-format off
        // |kth_poly_coeffs[i] = Σ{c in num_cosets} g^{k * (i + c * |coset|)} * a[i + c * |coset|]|
      // clang-format on
      // Where c represents the index of the coset being considered. multiplying
      // by g^{k * i} corresponds to the shift for just being in a different
      // coset.
      //
      // TODO(chokobole): Come back and improve the speed, and make this a more
      // 'normal'
      // See
      // https://github.com/arkworks-rs/algebra/blob/993a4e7ca4ac495c733397dcb7a881b53ab1b18d/poly/src/domain/utils.rs#L151
      // Cooley-Tukey. This appears to be an FFT of the polynomial
      // |P(x) = Σ{c in num_cosets} a[i + c |coset|] * x^c|
      // onto this coset.
      // However this is being evaluated in time O(N) instead of time
      // O(|coset|log(|coset|)). If this understanding is the case, its not
      // doing standard Cooley-Tukey. At the moment, this has time complexity
      // of at least 2 * N field mul's per thread, so we will be getting
      // pretty bad parallelism. Exact complexity per thread atm is
      // |2 * N + (N / num threads)log(N / num threads)| field muls.
      // Compare to the time complexity of serial is Nlog(N) field muls), with
      // log(N) in [15, 25]
      for (size_t i = 0; i < coset_size; ++i) {
        for (size_t c = 0; c < num_threads; ++c) {
          size_t idx = i + (c * coset_size);
          // t = the value of a corresponding to the ith element of
          // the sth coset.
          F t = a[idx] * elt;
          kth_poly_coeffs.at(i) += t;
          // elt = g^{k * idx}
          elt *= omega_step;
        }
        elt *= omega_k;
      }

      // Perform sub-FFT
      // Since the sub-FFT is mutative, after this point
      // |kth_poly_coeffs| should be renamed |kth_coset_evals|.
      SerialFFT(kth_poly_coeffs, new_omega, new_two_adicity);
    }

    // shuffle the values computed above into a
    // The evaluations of a should be ordered as (1, g, g², ...)
    for (size_t i = 0; i < a.NumElements(); ++i) {
      a.at(i) = tmp[i % num_cosets][i / num_cosets];
    }
  }
#endif
};

}  // namespace tachyon::math

#endif  // TACHYON_MATH_POLYNOMIALS_UNIVARIATE_MIXED_RADIX_EVALUATION_DOMAIN_H_
