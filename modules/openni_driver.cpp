/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2011 2011 Willow Garage, Inc.
 *    Suat Gedikli <gedikli@willowgarage.com>
 *
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 */
#include <ecto/ecto.hpp>
#include <openni_wrapper/openni_driver.h>
#include <openni_wrapper/openni_device.h>
#include <openni_wrapper/openni_image.h>
#include <openni_wrapper/openni_depth_image.h>
#include <openni_wrapper/openni_ir_image.h>
#include <openni_wrapper/synchronizer.h>
#include <iostream>
#include <string>
#include <map>
#include <XnCppWrapper.h>
#include <opencv2/opencv.hpp>
#include <boost/thread.hpp>
#include <sys/times.h>

#include "openni_wrapper/enums.hpp"
//#include "enums.hpp"

using namespace std;
using namespace openni_wrapper;
using namespace cv;
using namespace boost;
namespace bp = boost::python;
namespace ecto_openni
{
  bp::list
  device_list()
  {
    bp::list dl;
    OpenNIDriver& driver = OpenNIDriver::getInstance();
    size_t ndevices = driver.getNumberDevices();
    for (size_t i = 0; i < ndevices; i++)
    {
      boost::shared_ptr<OpenNIDevice> device = driver.getDeviceByIndex(i);
      std::string serial_number = device->getSerialNumber();
      int index = i;
      std::string vendor_name = device->getVendorName();
      int vendor_id = device->getVendorID();
      bp::dict dev;
      dev["index"] = i;
      dev["serial_number"] = serial_number;
      dev["vendor_name"] = vendor_name;
      dev["vendor_id"] = vendor_id;
      dl.append(dev);
    }
    return dl;
  }
  void
  wrap_openni_enumerate()
  {
    bp::def("enumerate_devices", device_list);
  }

  struct OpenNIStuff
  {
    OpenNIStuff(unsigned device_index, int width, int height, int dwidth, int dheight, int nFPS);
    void
    getLatest(StreamMode mode, bool registration, cv::Mat& depth, cv::Mat& image, cv::Mat& ir);
    void
    dataReady(StreamMode mode, unsigned long stamp);
    void
    start(StreamMode mode, bool registration);
    void
    imageCallback(boost::shared_ptr<Image> image, void* cookie);
    void
    irCallback(boost::shared_ptr<IRImage> image, void* cookie);
    void
    depthCallback(boost::shared_ptr<DepthImage> depth, void* cookie);
    void
    syncRGBCallback(const boost::shared_ptr<openni_wrapper::Image> &image,
                    const boost::shared_ptr<openni_wrapper::DepthImage> &depth_image);
    void
    syncIRCallback(const boost::shared_ptr<openni_wrapper::IRImage> &image,
                   const boost::shared_ptr<openni_wrapper::DepthImage> &depth_image);

    boost::shared_ptr<const OpenNIDevice>
    device() const
    {
      return devices_[selected_device_];
    }
    float
    getImageFocalLength() const
    {
      return device()->getImageFocalLength(device()->getImageOutputMode().nXRes);
    }
    float
    getDepthFocalLength() const
    {
      return device()->getImageFocalLength(device()->getDepthOutputMode().nXRes);
    }
    float
    getBaseline() const
    {
      return device()->getBaseline();
    }
    typedef map<string, Mat> DeviceImageMapT;
    DeviceImageMapT rgb_images_;
    DeviceImageMapT gray_images_;
    DeviceImageMapT ir_images_;
    DeviceImageMapT depth_images_;
    vector<boost::shared_ptr<OpenNIDevice> > devices_;
    unsigned selected_device_;
    boost::condition_variable cond;
    boost::mutex mut;
    int data_ready;
    bool registration_mode_;
    StreamMode stream_mode_;
    XnMapOutputMode rgb_output_mode_, depth_output_mode_;
    double timestamps[3];
  };

  OpenNIStuff::OpenNIStuff(unsigned device_index, int width, int height, int depth_width, int depth_height, int nFPS)
      :
        selected_device_(0),
        registration_mode_(false)
  {
    OpenNIDriver& driver = OpenNIDriver::getInstance();

    if (device_index >= driver.getNumberDevices())
    {
      std::stringstream ss;
      ss << "Index out of range." << driver.getNumberDevices() << " devices found.";
      throw std::runtime_error(ss.str());
    }
    boost::shared_ptr<OpenNIDevice> device = driver.getDeviceByIndex(device_index);
    cout << devices_.size() + 1 << ". device on bus: " << (int) device->getBus() << " @ " << (int) device->getAddress()
         << " with serial number: " << device->getSerialNumber() << "  " << device->getVendorName() << " : "
         << device->getProductName() << endl;
    selected_device_ = devices_.size();
    devices_.push_back(device);
    XnMapOutputMode mode;
    mode.nXRes = width;
    mode.nYRes = height;
    mode.nFPS = nFPS;
    if (device->hasImageStream())
    {
      if (!device->isImageModeSupported(mode))
      {
        std::stringstream ss;
        ss << "image stream mode " << mode.nXRes << " x " << mode.nYRes << " @ " << mode.nFPS << " not supported";
        throw std::runtime_error(ss.str());
      }
      rgb_images_[device->getConnectionString()] = Mat::zeros(height, width, CV_8UC3);
      gray_images_[device->getConnectionString()] = Mat::zeros(height, width, CV_8UC1);
      device->registerImageCallback(&OpenNIStuff::imageCallback, *this, &(*device));
    }

    if (device->hasIRStream())
    {
      if (!device->isImageModeSupported(mode))
      {
        std::stringstream ss;
        ss << "IR stream mode " << mode.nXRes << " x " << mode.nYRes << " @ " << mode.nFPS << " not supported";
        throw std::runtime_error(ss.str());
      }
      ir_images_[device->getConnectionString()] = Mat::zeros(height, width, CV_16UC1);
      device->registerIRCallback(&OpenNIStuff::irCallback, *this, &(*device));
    }

    if (device->hasDepthStream())
    {
      if (!device->isDepthModeSupported(mode))
      {
        std::stringstream ss;

        ss << "depth stream mode " << mode.nXRes << " x " << mode.nYRes << " @ " << mode.nFPS << " not supported"
           << endl;
        throw std::runtime_error(ss.str());
      }
      depth_images_[device->getConnectionString()] = Mat::zeros(height, width, CV_16UC1);
      device->registerDepthCallback(&OpenNIStuff::depthCallback, *this, &(*device));
    }
  }
  void
  OpenNIStuff::imageCallback(boost::shared_ptr<Image> image, void* cookie)
  {
    OpenNIDevice* device = static_cast<OpenNIDevice*>(cookie);
    Mat rgb = rgb_images_[device->getConnectionString()];

    unsigned char* buffer = (unsigned char*) (rgb.data);
    image->fillRGB(rgb.cols, rgb.rows, buffer, rgb.step);
    dataReady(RGB, image->getTimeStamp());
  }

  void
  OpenNIStuff::irCallback(boost::shared_ptr<IRImage> image, void* cookie)
  {
    OpenNIDevice* device = static_cast<OpenNIDevice*>(cookie);
    Mat ir = ir_images_[device->getConnectionString()];

    uint16_t* buffer = (uint16_t*) (ir.data);
    image->fillRaw(ir.cols, ir.rows, buffer, ir.step);
    dataReady(IR, image->getTimeStamp());
  }

  void
  OpenNIStuff::depthCallback(boost::shared_ptr<DepthImage> image, void* cookie)
  {
    OpenNIDevice* device = static_cast<OpenNIDevice*>(cookie);
    Mat depth = depth_images_[device->getConnectionString()];

    uint16_t* buffer = (uint16_t*) depth.data;
    image->fillDepthImageRaw(depth.cols, depth.rows, buffer, depth.step);
    dataReady(DEPTH, image->getTimeStamp());
  }

  void
  OpenNIStuff::dataReady(StreamMode mode, unsigned long stamp)
  {
    {
      boost::lock_guard<boost::mutex> lock(mut);
      data_ready |= mode;
      timestamps[int(log(double(mode)) / log(2.0))] = stamp * 1.0e-3; //to milliseconds
    }
    cond.notify_one();
  }

  void
  OpenNIStuff::getLatest(StreamMode mode, bool registration, cv::Mat& depth, cv::Mat& image, cv::Mat& ir)
  {
    const double time_diff_max = 20;
    std::string connection = devices_[selected_device_]->getConnectionString();
    if (stream_mode_ != mode || registration_mode_ != registration)
    {
      start(mode, registration);
    }

    boost::unique_lock<boost::mutex> lock(mut);
//awesome GOTO label.
    wait_for_data: while ((data_ready & mode) != mode)
    {
      cond.wait(lock);
    }
    bool check_sync = false;
    double depth_stamp = timestamps[int(log(double(DEPTH)) / log(2.0))];
    double rgb_stamp = timestamps[int(log(double(RGB)) / log(2.0))];
    double ir_stamp = timestamps[int(log(double(IR)) / log(2.0))];

    if (mode & DEPTH)
    {
      Mat depth_ = depth_images_[connection];
      depth_.copyTo(depth);
    }
    else
    {
      check_sync = false;
    }
    if (mode & IR)
    {
//    std::cout << "IR vs Depth:" << ir_stamp - depth_stamp << std::endl;
      Mat ir_ = ir_images_[connection];
      ir_.copyTo(ir);
      if (check_sync)
      {
        if (depth_stamp - ir_stamp > time_diff_max) //ir_stamp too old...
        {
          data_ready = data_ready ^ IR; //wipe out IR
          goto wait_for_data;
        }
        else if (ir_stamp - depth_stamp > time_diff_max) //depth too old.
        {
          data_ready = data_ready ^ DEPTH; //wipe out depth data
          goto wait_for_data;
        }
      }
    }
    if (mode & RGB)
    {
//    std::cout << "RGB vs Depth:" << rgb_stamp - depth_stamp << std::endl;
      Mat image_ = rgb_images_[connection];
      image_.copyTo(image);
      if (check_sync)
      {
        if (depth_stamp - rgb_stamp > time_diff_max) //rgb too old...
        {
          data_ready = data_ready ^ RGB; //wipe out RGB
          goto wait_for_data;
        }
        else if (rgb_stamp - depth_stamp > time_diff_max) //depth too old.
        {
          data_ready = data_ready ^ DEPTH; //wipe out DEPTH
          goto wait_for_data;
        }
      }
    }
    data_ready = 0;
  }

  void
  OpenNIStuff::start(StreamMode mode, bool registered)
  {
    boost::shared_ptr<OpenNIDevice> device = devices_[selected_device_];

    if (registered != registration_mode_ && device->isDepthRegistrationSupported())
    {
      device->setDepthRegistration(registered);
    }
    if (mode & DEPTH)
    {
      device->startDepthStream();
    }
    else
    {
      device->stopDepthStream();
    }

    if (mode & IR)
    {
      device->stopImageStream();
      device->startIRStream();
    }
    if (mode & RGB)
    {
      device->stopIRStream();
      device->startImageStream();
    }
    registration_mode_ = registered;
    stream_mode_ = mode;
    data_ready = 0;
  }
  using ecto::tendrils;
  struct OpenNICapture
  {

    static void
    declare_params(tendrils& p)
    {
      p.declare<StreamMode>("stream_mode", "The stream mode to capture. This is dynamic.");
      p.declare<bool>("registration", "Should the depth be registered?", false);
      p.declare<bool>("latched", "Should the output images be latched?", false);
    }

    static void
    declare_io(const tendrils& p, tendrils& i, tendrils& o)
    {
      o.declare<cv::Mat>("depth", "The depth stream.");
      o.declare<cv::Mat>("image", "The image stream.");
      o.declare<cv::Mat>("ir", "The IR stream.");
      o.declare<float>("focal_length_image", "The focal length of the image stream.");
      o.declare<float>("focal_length_depth", "The focal length of the depth stream.");
      o.declare<float>("baseline", "The base line of the openni camera.");
    }

    void
    configure(const tendrils& p, const tendrils& i, const tendrils& o)
    {
      connect(0, 640, 480, 30);
    }

    void
    connect(unsigned device_index, int width, int height, int nFPS)
    {
      device_.reset(new OpenNIStuff(device_index, width, height, width, height, nFPS));
    }

    int
    process(const tendrils&, const tendrils&)
    {
      //realloc every frame to avoid threading issues.
      cv::Mat depth, image, ir;
      device_->getLatest(*stream_mode_, *registration_, depth, image, ir);
      if (!depth.empty() || !*latched_)
        *depth_ = depth;
      if (!ir.empty() || !*latched_)
        *ir_ = ir;
      if (!image.empty() || !*latched_)
      {
        if (!image.empty())
          cv::cvtColor(image, image, CV_RGB2BGR);
        *image_ = image;
      }
      *focal_length_depth_ = device_->getDepthFocalLength();
      *focal_length_image_ = device_->getImageFocalLength();
      *baseline_ = device_->getBaseline();
      return ecto::OK;
    }
    ecto::spore<StreamMode> stream_mode_;
    ecto::spore<cv::Mat> depth_, ir_, image_;
    boost::shared_ptr<OpenNIStuff> device_;
    ecto::spore<bool> registration_, latched_;
    ecto::spore<float> focal_length_image_, focal_length_depth_, baseline_;
  };
}
ECTO_CELL(ecto_openni, ecto_openni::OpenNICapture, "OpenNICapture", "Raw data capture off of an OpenNI device.");
