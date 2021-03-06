#include "robot_localization/filter_common.h"
#include "robot_localization/ukf.h"

#include <XmlRpcException.h>

#include <sstream>
#include <iomanip>
#include <limits>

#include <Eigen/Cholesky>

#include <iostream>

#include <assert.h>

namespace RobotLocalization
{
  Ukf::Ukf(std::vector<double> args) :
    FilterBase(), // Must initialize filter base!
    uncorrected_(true)
  {
    assert(args.size() == 3);

    double alpha = args[0];
    double kappa = args[1];
    double beta = args[2];

    size_t sigmaCount = (STATE_SIZE << 1) + 1;
    sigmaPoints_.resize(sigmaCount, Eigen::VectorXd(STATE_SIZE));

    // Prepare constants
    lambda_ = alpha * alpha * (STATE_SIZE + kappa) - STATE_SIZE;

    stateWeights_.resize(sigmaCount);
    covarWeights_.resize(sigmaCount);

    stateWeights_[0] = lambda_ / (STATE_SIZE + lambda_);
    covarWeights_[0] = stateWeights_[0] + (1 - (alpha * alpha) + beta);
    sigmaPoints_[0].setZero();
    for(size_t i = 1; i < sigmaCount; ++i)
    {
      sigmaPoints_[i].setZero();
      stateWeights_[i] =  1 / (2 * (STATE_SIZE + lambda_));
      covarWeights_[i] = stateWeights_[i];
    }
  }

  Ukf::~Ukf()
  {
  }

  void Ukf::correct(const Measurement &measurement)
  {
    if (getDebug())
    {
      *debugStream_ << "---------------------- Ukf::correct ----------------------\n";
      *debugStream_ << "State is:\n";
      *debugStream_ << state_ << "\n";
      *debugStream_ << "Measurement is:\n";
      *debugStream_ << measurement.measurement_ << "\n";
      *debugStream_ << "Measurement covariance is:\n";
      *debugStream_ << measurement.covariance_ << "\n";
    }

    // In our implementation, it may be that after we call predict once, we call correct
    // several times in succession (multiple measurements with different time stamps). In
    // that event, the sigma points need to be updated to reflect the current state.
    if(!uncorrected_)
    {
      // Take the square root of a small fraction of the estimateErrorCovariance_ using LL' decomposition
      weightedCovarSqrt_ = ((STATE_SIZE + lambda_) * estimateErrorCovariance_).llt().matrixL();

      // Compute sigma points

      // First sigma point is the current state
      sigmaPoints_[0] = state_;

      // Next STATE_SIZE sigma points are state + weightedCovarSqrt_[ith column]
      // STATE_SIZE sigma points after that are state - weightedCovarSqrt_[ith column]
      for(size_t sigmaInd = 0; sigmaInd < STATE_SIZE; ++sigmaInd)
      {
        sigmaPoints_[sigmaInd + 1] = state_ + weightedCovarSqrt_.col(sigmaInd);
        sigmaPoints_[sigmaInd + 1 + STATE_SIZE] = state_ - weightedCovarSqrt_.col(sigmaInd);
      }
    }

    // We don't want to update everything, so we need to build matrices that only update
    // the measured parts of our state vector

    // First, determine how many state vector values we're updating
    std::vector<size_t> updateIndices;
    for (size_t i = 0; i < measurement.updateVector_.size(); ++i)
    {
      if (measurement.updateVector_[i])
      {
        // Handle nan and inf values in measurements
        if (std::isnan(measurement.measurement_(i)))
        {
          if (getDebug())
          {
            *debugStream_ << "Value at index " << i << " was nan. Excluding from update.\n";
          }
        }
        else if (std::isinf(measurement.measurement_(i)))
        {
          if (getDebug())
          {
            *debugStream_ << "Value at index " << i << " was inf. Excluding from update.\n";
          }
        }
        else
        {
          updateIndices.push_back(i);
        }
      }
    }

    if (getDebug())
    {
      *debugStream_ << "Update indices are:\n";
      *debugStream_ << updateIndices << "\n";
    }

    size_t updateSize = updateIndices.size();

    // Now set up the relevant matrices
    Eigen::VectorXd stateSubset(updateSize);                             // x (in most literature)
    Eigen::VectorXd measurementSubset(updateSize);                       // z
    Eigen::MatrixXd measurementCovarianceSubset(updateSize, updateSize); // R
    Eigen::MatrixXd stateToMeasurementSubset(updateSize, STATE_SIZE);    // H
    Eigen::MatrixXd kalmanGainSubset(STATE_SIZE, updateSize);            // K
    Eigen::VectorXd innovationSubset(updateSize);                        // z - Hx
    Eigen::VectorXd predictedMeasurement(updateSize);
    Eigen::VectorXd sigmaDiff(updateSize);
    Eigen::MatrixXd predictedMeasCovar(updateSize, updateSize);
    Eigen::MatrixXd crossCovar(STATE_SIZE, updateSize);

    std::vector<Eigen::VectorXd> sigmaPointMeasurements(sigmaPoints_.size(), Eigen::VectorXd(updateSize));

    stateSubset.setZero();
    measurementSubset.setZero();
    measurementCovarianceSubset.setZero();
    stateToMeasurementSubset.setZero();
    kalmanGainSubset.setZero();
    innovationSubset.setZero();
    predictedMeasurement.setZero();
    predictedMeasCovar.setZero();
    crossCovar.setZero();

    // Now build the sub-matrices from the full-sized matrices
    for (size_t i = 0; i < updateSize; ++i)
    {
      measurementSubset(i) = measurement.measurement_(updateIndices[i]);
      stateSubset(i) = state_(updateIndices[i]);

      for (size_t j = 0; j < updateSize; ++j)
      {
        measurementCovarianceSubset(i, j) = measurement.covariance_(updateIndices[i], updateIndices[j]);
      }

      // Handle negative (read: bad) covariances in the measurement. Rather
      // than exclude the measurement or make up a covariance, just take
      // the absolute value.
      if (measurementCovarianceSubset(i, i) < 0)
      {
        if (getDebug())
        {
          *debugStream_ << "WARNING: Negative covariance for index " << i << " of measurement (value is" << measurementCovarianceSubset(i, i)
            << "). Using absolute value...\n";
        }

        measurementCovarianceSubset(i, i) = ::fabs(measurementCovarianceSubset(i, i));
      }

      // If the measurement variance for a given variable is very
      // near 0 (as in e-50 or so) and the variance for that
      // variable in the covariance matrix is also near zero, then
      // the Kalman gain computation will blow up. Really, no
      // measurement can be completely without error, so add a small
      // amount in that case.
      if (measurementCovarianceSubset(i, i) < 1e-6)
      {
        measurementCovarianceSubset(i, i) = 1e-6;

        if (getDebug())
        {
          *debugStream_ << "WARNING: measurement had very small error covariance for index " << updateIndices[i] << ". Adding some noise to maintain filter stability.\n";
        }
      }
    }

    // The state-to-measurement function, h, will now be a measurement_size x full_state_size
    // matrix, with ones in the (i, i) locations of the values to be updated
    for (size_t i = 0; i < updateSize; ++i)
    {
      stateToMeasurementSubset(i, updateIndices[i]) = 1;
    }

    if (getDebug())
    {
      *debugStream_ << "Current state subset is:\n";
      *debugStream_ << stateSubset << "\n";
      *debugStream_ << "Measurement subset is:\n";
      *debugStream_ << measurementSubset << "\n";
      *debugStream_ << "Measurement covariance subset is:\n";
      *debugStream_ << measurementCovarianceSubset << "\n";
      *debugStream_ << "State-to-measurement subset is:\n";
      *debugStream_ << stateToMeasurementSubset << "\n";
    }

    // (1) Generate sigma points, use them to generate a predicted measurement
    for(size_t sigmaInd = 0; sigmaInd < sigmaPoints_.size(); ++sigmaInd)
    {
      sigmaPointMeasurements[sigmaInd] = stateToMeasurementSubset * sigmaPoints_[sigmaInd];
      predictedMeasurement += stateWeights_[sigmaInd] * sigmaPointMeasurements[sigmaInd];
    }

    // (2) Use the sigma point measurements and predicted measurement to compute a predicted
    // measurement covariance matrix P_zz and a state/measurement cross-covariance matrix P_xz.
    for(size_t sigmaInd = 0; sigmaInd < sigmaPoints_.size(); ++sigmaInd)
    {
      sigmaDiff = sigmaPointMeasurements[sigmaInd] - predictedMeasurement;
      predictedMeasCovar += covarWeights_[sigmaInd] * (sigmaDiff * sigmaDiff.transpose());
      crossCovar += covarWeights_[sigmaInd] * ((sigmaPoints_[sigmaInd] - state_) * sigmaDiff.transpose());
    }

    // (3) Compute the Kalman gain, making sure to use the actual measurement covariance: K = P_xz * (P_zz + R)^-1
    kalmanGainSubset = crossCovar * (predictedMeasCovar + measurementCovarianceSubset).inverse();

    // (4) Apply the gain to the difference between the actual and predicted measurements: x = x + K(z - z_hat)
    innovationSubset = (measurementSubset - predictedMeasurement);

    // Wrap angles in the innovation
    for (size_t i = 0; i < updateSize; ++i)
    {
      if (updateIndices[i] == StateMemberRoll || updateIndices[i] == StateMemberPitch || updateIndices[i] == StateMemberYaw)
      {
        if (innovationSubset(i) < -pi_)
        {
          innovationSubset(i) += tau_;
        }
        else if (innovationSubset(i) > pi_)
        {
          innovationSubset(i) -= tau_;
        }
      }
    }

    state_ = state_ + kalmanGainSubset * innovationSubset;

    // (5) Compute the new estimate error covariance P = P - (K * P_zz * K')
    estimateErrorCovariance_ = estimateErrorCovariance_.eval() - (kalmanGainSubset * predictedMeasCovar * kalmanGainSubset.transpose());

    wrapStateAngles();

    // Mark that we need to re-compute sigma points for successive corrections
    uncorrected_ = false;

    if (getDebug())
    {
      *debugStream_ << "Predicated measurement covariance is:\n";
      *debugStream_ << predictedMeasCovar << "\n";
      *debugStream_ << "Cross covariance is:\n";
      *debugStream_ << crossCovar << "\n";
      *debugStream_ << "Kalman gain subset is:\n";
      *debugStream_ << kalmanGainSubset << "\n";
      *debugStream_ << "Innovation:\n";
      *debugStream_ << innovationSubset << "\n\n";
      *debugStream_ << "Corrected full state is:\n";
      *debugStream_ << state_ << "\n";
      *debugStream_ << "Corrected full estimate error covariance is:\n";
      *debugStream_ << estimateErrorCovariance_ << "\n";
      *debugStream_ << "\n---------------------- /Ukf::correct ----------------------\n";
    }
  }

  void Ukf::predict(const double delta)
  {
    if (getDebug())
    {
      *debugStream_ << "---------------------- Ukf::predict ----------------------\n";
      *debugStream_ << "delta is " << delta << "\n";
      *debugStream_ << "state is " << state_ << "\n";
    }

    double roll = state_(StateMemberRoll);
    double pitch = state_(StateMemberPitch);
    double yaw = state_(StateMemberYaw);
    double xVel = state_(StateMemberVx);
    double yVel = state_(StateMemberVy);
    double zVel = state_(StateMemberVz);
    double xAcc = state_(StateMemberAx);
    double yAcc = state_(StateMemberAy);
    double zAcc = state_(StateMemberAz);

    // We'll need these trig calculations a lot.
    double cr = cos(roll);
    double cp = cos(pitch);
    double cy = cos(yaw);
    double sr = sin(roll);
    double sp = sin(pitch);
    double sy = sin(yaw);

    // Prepare the transfer function
    transferFunction_(StateMemberX, StateMemberVx) = cy * cp * delta;
    transferFunction_(StateMemberX, StateMemberVy) = (cy * sp * sr - sy * cr) * delta;
    transferFunction_(StateMemberX, StateMemberVz) = (cy * sp * cr + sy * sr) * delta;
    transferFunction_(StateMemberX, StateMemberAx) = 0.5 * transferFunction_(StateMemberX, StateMemberVx) * delta;
    transferFunction_(StateMemberX, StateMemberAy) = 0.5 * transferFunction_(StateMemberX, StateMemberVy) * delta;
    transferFunction_(StateMemberX, StateMemberAz) = 0.5 * transferFunction_(StateMemberX, StateMemberVz) * delta;
    transferFunction_(StateMemberY, StateMemberVx) = sy * cp * delta;
    transferFunction_(StateMemberY, StateMemberVy) = (sy * sp * sr + cy * cr) * delta;
    transferFunction_(StateMemberY, StateMemberVz) = (sy * sp * cr - cy * sr) * delta;
    transferFunction_(StateMemberY, StateMemberAx) = 0.5 * transferFunction_(StateMemberY, StateMemberVx) * delta;
    transferFunction_(StateMemberY, StateMemberAy) = 0.5 * transferFunction_(StateMemberY, StateMemberVy) * delta;
    transferFunction_(StateMemberY, StateMemberAz) = 0.5 * transferFunction_(StateMemberY, StateMemberVz) * delta;
    transferFunction_(StateMemberZ, StateMemberVx) = -sp * delta;
    transferFunction_(StateMemberZ, StateMemberVy) = cp * sr * delta;
    transferFunction_(StateMemberZ, StateMemberVz) = cp * cr * delta;
    transferFunction_(StateMemberZ, StateMemberAx) = 0.5 * transferFunction_(StateMemberZ, StateMemberVx) * delta;
    transferFunction_(StateMemberZ, StateMemberAy) = 0.5 * transferFunction_(StateMemberZ, StateMemberVy) * delta;
    transferFunction_(StateMemberZ, StateMemberAz) = 0.5 * transferFunction_(StateMemberZ, StateMemberVz) * delta;
    transferFunction_(StateMemberRoll, StateMemberVroll) = delta;
    transferFunction_(StateMemberPitch, StateMemberVpitch) = delta;
    transferFunction_(StateMemberYaw, StateMemberVyaw) = delta;
    transferFunction_(StateMemberVx, StateMemberAx) = delta;
    transferFunction_(StateMemberVy, StateMemberAy) = delta;
    transferFunction_(StateMemberVz, StateMemberAz) = delta;

    // (1) Take the square root of a small fraction of the estimateErrorCovariance_ using LL' decomposition
    weightedCovarSqrt_ = ((STATE_SIZE + lambda_) * estimateErrorCovariance_).llt().matrixL();

    // (2) Compute sigma points *and* pass them through the transfer function to save
    // the extra loop

    // First sigma point is the current state
    sigmaPoints_[0] = transferFunction_ * state_;

    // Next STATE_SIZE sigma points are state + weightedCovarSqrt_[ith column]
    // STATE_SIZE sigma points after that are state - weightedCovarSqrt_[ith column]
    for(size_t sigmaInd = 0; sigmaInd < STATE_SIZE; ++sigmaInd)
    {
      sigmaPoints_[sigmaInd + 1] = transferFunction_ * (state_ + weightedCovarSqrt_.col(sigmaInd));
      sigmaPoints_[sigmaInd + 1 + STATE_SIZE] = transferFunction_ * (state_ - weightedCovarSqrt_.col(sigmaInd));
    }

    // (3) Sum the weighted sigma points to generate a new state prediction
    state_.setZero();
    for(size_t sigmaInd = 0; sigmaInd < sigmaPoints_.size(); ++sigmaInd)
    {
      state_ += stateWeights_[sigmaInd] * sigmaPoints_[sigmaInd];
    }

    // (4) Now us the sigma points and the predicted state to compute a predicted covariance
    estimateErrorCovariance_.setZero();
    Eigen::VectorXd sigmaDiff(STATE_SIZE);
    for(size_t sigmaInd = 0; sigmaInd < sigmaPoints_.size(); ++sigmaInd)
    {
      sigmaDiff = (sigmaPoints_[sigmaInd] - state_);
      estimateErrorCovariance_ += covarWeights_[sigmaInd] * (sigmaDiff * sigmaDiff.transpose());
    }

    // (5) Not strictly in the theoretical UKF formulation, but necessary here
    // to ensure that we actually incorporate the processNoiseCovariance_
    estimateErrorCovariance_ += delta * processNoiseCovariance_;

    // Keep the angles bounded
    wrapStateAngles();

    // Mark that we can keep these sigma points
    uncorrected_ = true;

    if (getDebug())
    {
      *debugStream_ << "Predicted state is:\n";
      *debugStream_ << state_ << "\n";
      *debugStream_ << "Predicted estimate error covariance is:\n";
      *debugStream_ << estimateErrorCovariance_ << "\n";
      *debugStream_ << "\n--------------------- /Ukf::predict ----------------------\n";
    }
  }

}
