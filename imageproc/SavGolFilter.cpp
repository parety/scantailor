/*
    Scan Tailor - Interactive post-processing tool for scanned pages.
    Copyright (C) 2007-2008  Joseph Artsimovich <joseph_a@mail.ru>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "SavGolFilter.h"
#include "Grayscale.h"
#include <QImage>
#include <QSize>
#include <QPoint>
#include <QtGlobal>
#include <algorithm>
#include <vector>
#include <stdexcept>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <assert.h>

namespace imageproc
{

namespace
{

class SavGolKernel
{
public:
	SavGolKernel(int order, QSize const& size, QPoint const& origin);
	
	void recalcForOrigin(QPoint const& origin);
	
	void convolve(uint8_t* dst, uint8_t const* src_top_left, int src_bpl) const;
private:
	struct Rotation
	{
		double sin;
		double cos;
		
		Rotation(double s, double c) : sin(s), cos(c) {}
	};
	
	void QR();
	
	/**
	 * A matrix of m_numDataPoints rows and m_numVars columns.
	 * Stored row by row.
	 */
	std::vector<double> m_equations;
	
	/**
	 * The data points, in the same order as rows in m_equations.
	 */
	std::vector<double> m_dataPoints;
	
	/**
	 * The polynomial coefficients of size m_numVars.  Only exists to save
	 * one allocation when recalculating the kernel for different data points.
	 */
	std::vector<double> m_coeffs;
	
	/**
	 * The rotations applied to m_equations as part of QR factorization.
	 * Later these same rotations are applied to a copy of m_dataPoints.
	 * We could avoid storing rotations and rotate m_dataPoints on the fly,
	 * but in that case we would have to rotate m_equations again when
	 * recalculating the kernel for different data points.
	 */
	std::vector<Rotation> m_rotations;
	
	/**
	 * The convolution kernel of size m_numDataPoints.
	 */
	std::vector<double> m_kernel;
	
	/**
	 * The order of the polynomial.
	 */
	int m_order;
	
	/**
	 * The width of the convolution kernel.
	 */
	int m_width;
	
	/**
	 * The height of the convolution kernel.
	 */
	int m_height;
	
	/**
	 * The number of polynomial coefficients.  This is equal to (order + 1)^2.
	 */
	int m_numVars;
	
	/**
	 * The number of data points.  This corresponds to the number of items
	 * in the convolution kernel.
	 */
	int m_numDataPoints;
};

SavGolKernel::SavGolKernel(int const order, QSize const& size, QPoint const& origin)
:	m_order(order),
	m_width(size.width()),
	m_height(size.height()),
	m_numVars((order + 1) * (order + 1)),
	m_numDataPoints(size.width() * size.height())
{
	assert(order > 0);
	assert(m_numVars <= m_numDataPoints);
	
	// Allocate memory.
	m_dataPoints.resize(m_numDataPoints, 0.0);
	m_coeffs.resize(m_numVars);
	m_kernel.resize(m_numDataPoints);
	
	// Prepare equations.
	m_equations.reserve(m_numVars * m_numDataPoints);
	for (int y = 1; y <= m_height; ++y) {
		for (int x = 1; x <= m_width; ++x) {
			double pow1 = 1.0;
			for (int i = 0; i <= order; ++i, pow1 *= y) {
				double pow2 = pow1;
				for (int j = 0; j <= order; ++j, pow2 *= x) {
					m_equations.push_back(pow2);
				}
			}
		}
	}
	
	QR();
	recalcForOrigin(origin);
}

/**
 * Perform a QR factorization of m_equations by Givens rotations.
 * We store R in place of m_equations, and we don't store Q anywhere,
 * but we do store the rotations in the order they were performed.
 */
void
SavGolKernel::QR()
{
	m_rotations.clear();
	m_rotations.reserve(
		(m_numVars * (m_numVars - 1) / 2
		+ (m_numDataPoints - m_numVars) * m_numVars)
	);
	
	int jj = 0; // j * m_numVars + j
	for (int j = 0; j < m_numVars; ++j, jj += m_numVars + 1) {
		int ij = jj + m_numVars; // i * m_numVars + j
		for (int i = j + 1; i < m_numDataPoints; ++i, ij += m_numVars) {
			double const a = m_equations[jj];
			double const b = m_equations[ij];
			double const radius = sqrt(a*a + b*b);
			double const cos = a / radius;
			double const sin = b / radius;
			
			m_rotations.push_back(Rotation(sin, cos));
			m_equations[jj] = radius;
			m_equations[ij] = 0.0;
			
			int ik = ij + 1; // i * m_numVars + k
			int jk = jj + 1; // j * m_numVars + k
			for (int k = j + 1; k < m_numVars; ++k, ++ik, ++jk) {
				double const temp = cos * m_equations[jk] + sin * m_equations[ik];
				m_equations[ik] = cos * m_equations[ik] - sin * m_equations[jk];
				m_equations[jk] = temp;
			}
		}
	}
}

void
SavGolKernel::recalcForOrigin(QPoint const& origin)
{
	std::fill(m_dataPoints.begin(), m_dataPoints.end(), 0.0);
	m_dataPoints[origin.y() * m_width + origin.x()] = 1.0;
	
	// Rotate data points.
	double* const dp = &m_dataPoints[0];
	std::vector<Rotation>::const_iterator rot(m_rotations.begin());
	for (int j = 0; j < m_numVars; ++j) {
		for (int i = j + 1; i < m_numDataPoints; ++i, ++rot) {
			double const temp = rot->cos * dp[j] + rot->sin * dp[i];
			dp[i] = rot->cos * dp[i] - rot->sin * dp[j];
			dp[j] = temp;
		}
	}
	
	// Solve R*x = d, by back-substitution.
	int ii = m_numVars * m_numVars - 1; // i * m_numVars + i
	for (int i = m_numVars - 1; i >= 0; --i, ii -= m_numVars + 1) {
		double sum = dp[i];
		int ik = ii + 1;
		for (int k = i + 1; k < m_numVars; ++k, ++ik) {
			sum -= m_equations[ik] * m_coeffs[k];
		}
		
		assert(m_equations[ii] != 0.0);
		m_coeffs[i] = sum / m_equations[ii];
	}
	
	int ki = 0;
	for (int y = 1; y <= m_height; ++y) {
		for (int x = 1; x <= m_width; ++x) {
			double sum = 0.0;
			double pow1 = 1.0;
			int ci = 0;
			for (int i = 0; i <= m_order; ++i, pow1 *= y) {
				double pow2 = pow1;
				for (int j = 0; j <= m_order; ++j, pow2 *= x) {
					sum += pow2 * m_coeffs[ci];
					++ci;
				}
			}
			m_kernel[ki] = sum;
			++ki;
		}
	}
}

void
SavGolKernel::convolve(uint8_t* dst, uint8_t const* src_top_left, int src_bpl) const
{
	uint8_t const* p_src = src_top_left;
	double const* p_kernel = &m_kernel[0];
	double sum = 0.0;
	
	for (int y = 0; y < m_height; ++y, p_src += src_bpl) {
		for (int x = 0; x < m_width; ++x) {
			sum += p_src[x] * *p_kernel;
			++p_kernel;
		}
	}
	
	int const val = static_cast<int>(sum);
	*dst = static_cast<uint8_t>(qBound(0, val, 255));
}

QImage savGolFilterGrayToGray(
	QImage const& src, QSize const& window_size, int const order)
{
	int const width = src.width();
	int const height = src.height();
	
	int const kw = window_size.width();
	int const kh = window_size.height();
	
	if (kw > width || kh > height) {
		return src;
	}
	
	int const k_top = kh / 2;
	int const k_bottom = kh - k_top - 1;
	int const k_left = kw / 2;
	int const k_right = kw - k_left - 1;
	
	uint8_t const* const src_data = src.bits();
	int const src_bpl = src.bytesPerLine();
	
	QImage dst(width, height, QImage::Format_Indexed8);
	dst.setColorTable(createGrayscalePalette());
	
	uint8_t* const dst_data = dst.bits();
	int const dst_bpl = dst.bytesPerLine();

	// Top-left corner.
	QPoint origin(0, 0);
	uint8_t* dst_line = dst_data;
	SavGolKernel kernel(order, window_size, origin);
	for (int y = 0; y < k_top; ++y, dst_line += dst_bpl) {
		origin.setY(y);
		for (int x = 0; x < k_left; ++x) {
			origin.setX(x);
			kernel.recalcForOrigin(origin);
			kernel.convolve(dst_line + x, src_data, src_bpl);
		}
	}
	
	// Top area between two corners.
	origin.setX(kw / 2);
	dst_line = dst_data;
	for (int y = 0; y < k_top; ++y, dst_line += dst_bpl) {
		origin.setY(y);
		kernel.recalcForOrigin(origin);
		for (int x = k_left; x < width - k_right; ++x) {
			kernel.convolve(dst_line + x, src_data - k_left + x, src_bpl);
		}
	}
	
	// Top-right corner.
	origin.setY(0);
	dst_line = dst_data;
	for (int y = 0; y < k_top; ++y, dst_line += dst_bpl) {
		origin.setX(kw - 1);
		for (int x = width - k_right; x < width; ++x) {
			kernel.recalcForOrigin(origin);
			kernel.convolve(dst_line + x, src_data + width - kw, src_bpl);
			origin.rx() -= 1;
		}
		origin.ry() += 1;
	}
	
	// Central area.
	origin.setX(kw / 2);
	origin.setY(kh / 2);
	kernel.recalcForOrigin(origin);
	uint8_t const* src_line = src_data - k_left;	
	dst_line = dst_data + dst_bpl * k_top;
	for (int y = k_top; y < height - k_bottom; ++y) {
		for (int x = k_left; x < width - k_right; ++x) {
			kernel.convolve(dst_line + x, src_line + x, src_bpl);
		}
		src_line += src_bpl;
		dst_line += dst_bpl;
	}

	// Left area between two corners.
	origin.setX(0);
	origin.setY(kh / 2);
	for (int x = 0; x < k_left; ++x) {
		src_line = src_data;
		dst_line = dst_data + dst_bpl * k_top;
		
		kernel.recalcForOrigin(origin);
		for (int y = k_top; y < height - k_bottom; ++y) {
			kernel.convolve(dst_line + x, src_line, src_bpl);
			src_line += src_bpl;
			dst_line += dst_bpl;
		}
		origin.rx() += 1;
	}
	
	// Right area between two corners.
	origin.setX(kw / 2 + 1);
	origin.setY(kh / 2);
	for (int x = width - k_right; x < width; ++x) {
		src_line = src_data + width - kw;
		dst_line = dst_data + dst_bpl * k_top;
		
		kernel.recalcForOrigin(origin);
		for (int y = k_top; y < height - k_bottom; ++y) {
			kernel.convolve(dst_line + x, src_line, src_bpl);
			src_line += src_bpl;
			dst_line += dst_bpl;
		}
		origin.rx() += 1;
	}
	
	// Bottom-left corner.
	origin.setY(kh / 2 + 1);
	src_line = src_data + src_bpl * (height - kh);
	dst_line = dst_data + dst_bpl * (height - k_bottom);
	for (int y = height - k_bottom; y < height; ++y, dst_line += dst_bpl) {
		origin.setX(0);
		for (int x = 0; x < k_left; ++x) {
			origin.setX(x);
			kernel.recalcForOrigin(origin);
			kernel.convolve(dst_line + x, src_line, src_bpl);
			origin.rx() += 1;
		}
		origin.ry() += 1;
	}
	
	// Bottom area between two corners.
	origin.setX(kw / 2);
	origin.setY(kh / 2 + 1);
	src_line = src_data + src_bpl * (height - kh) - origin.x();
	dst_line = dst_data + dst_bpl * (height - k_bottom);
	for (int y = height - k_bottom; y < height; ++y, dst_line += dst_bpl) {
		kernel.recalcForOrigin(origin);
		for (int x = k_left; x < width - k_right; ++x) {
			kernel.convolve(dst_line + x, src_line + x, src_bpl);
		}
		origin.ry() += 1;
	}
	
	// Bottom-right corner.
	origin.setY(kh / 2 + 1);
	src_line = src_data + src_bpl * (height - kh) + (width - kw);
	dst_line = dst_data + dst_bpl * (height - k_bottom);
	for (int y = height - k_bottom; y < height; ++y, dst_line += dst_bpl) {
		origin.setX(kw / 2 + 1);
		for (int x = width - k_right; x < width; ++x) {
			kernel.recalcForOrigin(origin);
			kernel.convolve(dst_line + x, src_line, src_bpl);
			origin.rx() += 1;
		}
		origin.ry() += 1;
	}
	
	return dst;
}

} // anonymous namespace


QImage savGolFilter(
	QImage const& src, QSize const& window_size, int const order)
{
	if (window_size.isEmpty()) {
		throw std::invalid_argument("savGolFilter: invalid window size");
	}
	if ((order + 1) * (order + 1) >
			window_size.width() * window_size.height()) {
		throw std::invalid_argument(
			"savGolFilter: order is too big for that window");
	}
	
	return savGolFilterGrayToGray(toGrayscale(src), window_size, order);
}

} // namespace imageproc