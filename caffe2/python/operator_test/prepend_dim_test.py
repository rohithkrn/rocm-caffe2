# Copyright (c) 2016-present, Facebook, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##############################################################################

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
from __future__ import unicode_literals
import numpy as np

from caffe2.python import core, workspace
from caffe2.python.test_util import TestCase
from caffe2.proto import caffe2_pb2


class TestPrependDim(TestCase):
    def _test_fwd_bwd(self):
        old_shape = (128, 2, 4)
        new_shape = (8, 16, 2, 4)
        X = np.random.rand(*old_shape).astype(np.float32)
        Y = np.random.rand(*new_shape).astype(np.float32)

        net = core.Net('net')

        net.GivenTensorFill([], 'X', shape=old_shape, values=X.flatten())
        net.GivenTensorFill([], 'Y', shape=new_shape, values=Y.flatten())

        net.PrependDim(['X'], ['X_out'], dim_size=8)
        net.DotProduct(['X_out', 'Y'], 'Z')
        net.AddGradientOperators(['Z'])

        workspace.RunNetOnce(net)

        X_out = workspace.FetchBlob('X_out')
        X_grad = workspace.FetchBlob('X_grad')
        Y_grad = workspace.FetchBlob('Y_grad')

        # Check the shape of the gradient
        np.testing.assert_array_equal(X_out.shape, Y.shape)
        np.testing.assert_array_equal(X_grad.shape, X.shape)
        np.testing.assert_array_equal(Y_grad.shape, Y.shape)

    def test_prepend_dim(self):
        devices = [core.DeviceOption(caffe2_pb2.CPU, 0)]
        if workspace.NumGpuDevices() > 0:
            devices.append(core.DeviceOption(caffe2_pb2.HIP, 0) if workspace.has_hip else core.DeviceOption(caffe2_pb2.CUDA, 0))

        for device_opt in devices:
            with core.DeviceScope(device_opt):
                self._test_fwd_bwd()


if __name__ == "__main__":
    import unittest
    unittest.main()
