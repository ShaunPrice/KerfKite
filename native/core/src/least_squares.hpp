// Linear least squares by the singular value decomposition, for the sketch constraint solver. Internal to the
// core, and written here rather than taken from a library: LAPACK is what NumPy uses.
//
// The decomposition is one-sided Jacobi (Hestenes): the columns of a matrix are turned in pairs until they are
// orthogonal, which gives singular values to high relative accuracy. It is applied to whichever of A and its
// transpose has fewer columns, so a solver's Jacobian of r relations and c free coordinates costs about
// min(r, c)^2 max(r, c) a sweep. The answers are those of numpy.linalg.lstsq and matrix_rank to rounding, not
// to the bit: LAPACK's bidiagonalisation takes another path to the same values.
#ifndef KERFKITE_LEAST_SQUARES_HPP
#define KERFKITE_LEAST_SQUARES_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace kerfkite {
namespace detail {

// A matrix of rows.
using Matrix = std::vector<std::vector<double>>;

// A = U diag(sigma) V^T, with U of rows × k and V of columns × k, k = min(rows, columns), both stored by column.
struct SingularValues {
    std::vector<double> sigma;
    std::vector<std::vector<double>> u, v;
};

// One-sided Jacobi on the columns of b (rows × columns): b V = W with orthogonal columns, so b = U Σ V^T.
inline SingularValues jacobi(std::vector<std::vector<double>> w /* columns of b */, size_t rows) {
    const size_t columns = w.size();
    std::vector<std::vector<double>> v(columns, std::vector<double>(columns, 0.0));
    for (size_t i = 0; i < columns; ++i) v[i][i] = 1.0;
    const double eps = std::numeric_limits<double>::epsilon();
    for (int sweep = 0; sweep < 80; ++sweep) {
        bool turned = false;
        for (size_t p = 0; p + 1 < columns; ++p)
            for (size_t q = p + 1; q < columns; ++q) {
                double alpha = 0, beta = 0, gamma = 0;
                for (size_t i = 0; i < rows; ++i) {
                    alpha += w[p][i] * w[p][i];
                    beta += w[q][i] * w[q][i];
                    gamma += w[p][i] * w[q][i];
                }
                if (gamma == 0 || std::fabs(gamma) <= eps * std::sqrt(alpha * beta)) continue;
                turned = true;
                const double zeta = (beta - alpha) / (2 * gamma);
                const double t = (zeta >= 0 ? 1.0 : -1.0) / (std::fabs(zeta) + std::sqrt(1 + zeta * zeta));
                const double c = 1 / std::sqrt(1 + t * t), s = c * t;
                for (size_t i = 0; i < rows; ++i) {
                    const double x = w[p][i], y = w[q][i];
                    w[p][i] = c * x - s * y;
                    w[q][i] = s * x + c * y;
                }
                for (size_t i = 0; i < columns; ++i) {
                    const double x = v[p][i], y = v[q][i];
                    v[p][i] = c * x - s * y;
                    v[q][i] = s * x + c * y;
                }
            }
        if (!turned) break;
    }
    SingularValues result;
    result.v = std::move(v);
    for (size_t k = 0; k < columns; ++k) {
        double norm = 0;
        for (size_t i = 0; i < rows; ++i) norm += w[k][i] * w[k][i];
        norm = std::sqrt(norm);
        result.sigma.push_back(norm);
        std::vector<double> u(rows, 0.0);
        if (norm > 0)
            for (size_t i = 0; i < rows; ++i) u[i] = w[k][i] / norm;
        result.u.push_back(std::move(u));
    }
    return result;
}

// The singular value decomposition of a (rows × columns).
inline SingularValues decompose(const Matrix &a, size_t rows, size_t columns) {
    if (columns <= rows) {
        std::vector<std::vector<double>> w(columns, std::vector<double>(rows));
        for (size_t i = 0; i < rows; ++i)
            for (size_t j = 0; j < columns; ++j) w[j][i] = a[i][j];
        return jacobi(std::move(w), rows);
    }
    // a^T = U Σ V^T, so a = V Σ U^T: the factors trade places.
    std::vector<std::vector<double>> w(rows, std::vector<double>(columns));
    for (size_t i = 0; i < rows; ++i)
        for (size_t j = 0; j < columns; ++j) w[i][j] = a[i][j];
    SingularValues transposed = jacobi(std::move(w), columns);
    std::swap(transposed.u, transposed.v);
    return transposed;
}

// numpy.linalg.lstsq(a, b, rcond)[0]: the least-squares solution of least norm, singular values at or below
// rcond times the largest taken as zero.
inline std::vector<double> least_squares(const Matrix &a, size_t rows, size_t columns, const std::vector<double> &b, double rcond) {
    std::vector<double> x(columns, 0.0);
    if (!rows || !columns) return x;
    const SingularValues f = decompose(a, rows, columns);
    const double largest = f.sigma.empty() ? 0.0 : *std::max_element(f.sigma.begin(), f.sigma.end());
    for (size_t k = 0; k < f.sigma.size(); ++k) {
        if (!(f.sigma[k] > rcond * largest)) continue;
        double projection = 0;
        for (size_t i = 0; i < rows; ++i) projection += f.u[k][i] * b[i];
        const double scale = projection / f.sigma[k];
        for (size_t j = 0; j < columns; ++j) x[j] += scale * f.v[k][j];
    }
    return x;
}

// numpy.linalg.matrix_rank(a, tol): the singular values above tol.
inline size_t matrix_rank(const Matrix &a, size_t rows, size_t columns, double tol) {
    if (!rows || !columns) return 0;
    const SingularValues f = decompose(a, rows, columns);
    return static_cast<size_t>(std::count_if(f.sigma.begin(), f.sigma.end(), [tol](double s) { return s > tol; }));
}

}  // namespace detail
}  // namespace kerfkite

#endif
