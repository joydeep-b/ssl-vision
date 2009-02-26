//========================================================================
//  This software is free: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License Version 3,
//  as published by the Free Software Foundation.
//
//  This software is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  Version 3 in the file COPYING that came with this distribution.
//  If not, see <http://www.gnu.org/licenses/>.
//========================================================================
/*!
  \file    plugin_detect_balls.cpp
  \brief   C++ Implementation: plugin_detect_balls
  \author  Author Name, 2009
*/
//========================================================================
#include "plugin_detect_balls.h"

PluginDetectBalls::PluginDetectBalls(FrameBuffer * _buffer, LUT3D * lut, const CameraParameters& camera_params, const RoboCupField& field)
 : VisionPlugin(_buffer), camera_parameters(camera_params), field(field)
{
  _lut=lut;

  //read-out important LUT data:
  histogram = new CMVision::Histogram(_lut->getChannelCount());
  color_id_orange = _lut->getChannelID("Orange");
  if (color_id_orange == -1) printf("WARNING color label 'Orange' not defined in LUT!!!\n");
  color_id_pink = _lut->getChannelID("Pink");
  if (color_id_pink == -1) printf("WARNING color label 'Pink' not defined in LUT!!!\n");
  color_id_yellow = _lut->getChannelID("Yellow");
  if (color_id_yellow == -1) printf("WARNING color label 'Yellow' not defined in LUT!!!\n");
  color_id_field = _lut->getChannelID("Field Green");
  if (color_id_field == -1) printf("WARNING color label 'Field Green' not defined in LUT!!!\n");


  _settings=new VarList("Ball Detection");

  _settings->addChild(_max_balls = new VarInt("Max Ball Count",1));
  _settings->addChild(_color_label = new VarString("Ball Color","Orange"));

  _settings->addChild(_filter_general = new VarList("Ball Properties"));
    _filter_general->addChild(_ball_z_height = new VarDouble("Ball Z-Height", 30.0));
    _filter_general->addChild(_ball_max_speed = new VarDouble("Max Speed (mm/s)", 10000.0));
    _filter_general->addChild(_ball_min_width = new VarInt("Min Width (pixels)", 3));
    _filter_general->addChild(_ball_max_width = new VarInt("Max Width (pixels)", 30));
    _filter_general->addChild(_ball_min_height = new VarInt("Min Height (pixels)", 3));
    _filter_general->addChild(_ball_max_height = new VarInt("Max Height (pixels)", 30));
    _filter_general->addChild(_ball_min_area = new VarInt("Min Area (sq-pixels)", 9));
    _filter_general->addChild(_ball_max_area = new VarInt("Max Area (sq-pixels)", 100));    

  _settings->addChild(_filter_gauss = new VarList("Gaussian Size Filter"));
    _filter_gauss->addChild(_ball_gauss_enabled = new VarBool("Enable Filter",true));
    _filter_gauss->addChild(_ball_gauss_min = new VarInt("Expected Min Area (sq-pixels)", 30));
    _filter_gauss->addChild(_ball_gauss_max = new VarInt("Expected Max Area (sq-pixels)", 40));
    _filter_gauss->addChild(_ball_gauss_var = new VarDouble("Expected Area Variance", 20.0));

  _settings->addChild(_filter_too_near_robot = new VarList("Near Robot Filter"));
    _filter_too_near_robot->addChild(_ball_too_near_robot_enabled = new VarBool("Enable Filter",true));
    _filter_too_near_robot->addChild(_ball_too_near_robot_dist = new VarDouble("Distance (mm)",55));

  _settings->addChild(_filter_histogram = new VarList("Histogram Filter"));
    _filter_histogram->addChild(_ball_histogram_enabled = new VarBool("Enable Filter",true));
    _filter_histogram->addChild(_ball_histogram_min_greenness = new VarDouble("Min Greenness",0.5));
    _filter_histogram->addChild(_ball_histogram_max_markeryness = new VarDouble("Max Markeryness",2.0));

  _settings->addChild(_filter_geometry = new VarList("Geometry Filters"));
    _filter_geometry->addChild(_ball_on_field_filter = new VarBool("Ball-In-Field Filter",true));  
    _filter_geometry->addChild(_ball_in_goal_filter = new VarBool("Ball-In-Goal Filter",true));

}


PluginDetectBalls::~PluginDetectBalls()
{
  delete histogram;
}


VarList * PluginDetectBalls::getSettings() {
  return _settings;
}

string PluginDetectBalls::getName() {
  return "DetectBalls";
}

bool PluginDetectBalls::checkHistogram(const Image<raw8> * image, const CMVision::Region * reg, double min_greenness, double max_markeryness) {
  static const int PixelRadius = 4;

  histogram->clear();

  int num = histogram->addBox(image, reg->x1 - PixelRadius, reg->y1 - PixelRadius,
                    reg->x2 + PixelRadius, reg->y2 + PixelRadius);


  float pf = (float)(histogram->getChannel(color_id_pink)) / (float)(histogram->getChannel(color_id_orange));
  float yf = (float)(histogram->getChannel(color_id_yellow)) / (float)(histogram->getChannel(color_id_orange));
  float markeryness = (pf + 1) * (yf + 1) - 1;
  float greenness = (float)(histogram->getChannel(color_id_field)) / (((float)(num - histogram->getChannel(color_id_orange))) + 1E-6);

  printf("markeryness: %f\n",markeryness);
  if(greenness   > min_greenness) return(true);
  if(markeryness > max_markeryness) return(false);
  return(true);
}

ProcessResult PluginDetectBalls::process(FrameData * data, RenderOptions * options)
{
  (void)options;
  if (data==0) return ProcessingFailed;

  SSL_DetectionFrame * detection_frame = 0;

  detection_frame=(SSL_DetectionFrame *)data->map.get("ssl_detection_frame");
  if (detection_frame == 0) detection_frame=new SSL_DetectionFrame();

  int color_id_ball = _lut->getChannelID(_color_label->getString());
  if (color_id_ball == -1) {
    printf("Unknown Ball Detection Color Label: '%s'\nAborting Plugin!\n",_color_label->getString().c_str());
    return ProcessingFailed;
  }

  //delete any previous detection results:
  detection_frame->clear_balls();

  //initialize filter:
  filter.setWidth(_ball_min_width->getInt(),_ball_max_width->getInt());
  filter.setHeight(_ball_min_height->getInt(),_ball_max_height->getInt());
  filter.setArea(_ball_min_area->getInt(),_ball_max_area->getInt());
  field_filter.update(field);

  //copy all vartypes to local variables for faster repeated lookup:
  bool filter_ball_in_field = _ball_on_field_filter->getBool();
  bool filter_ball_in_goal  = _ball_in_goal_filter->getBool();
  bool filter_ball_histogram = _ball_histogram_enabled->getBool();
  if (filter_ball_histogram) {
    if (color_id_ball != color_id_orange) {
      printf("Warning: ball histogram check is only configured for orange balls!\n");
      printf("Please disable the histogram check in the Ball Detection Plugin settings\n");
    }
    if (color_id_pink==-1 || color_id_orange==-1 || color_id_yellow==-1 || color_id_field==-1) {
      printf("WARNING: some LUT color labels where undefined for the ball detection plugin\n");
      printf("         Disabling histogram check!\n");
      filter_ball_histogram=false;
    }
  }
  
  double min_greenness = _ball_histogram_min_greenness->getDouble();
  double max_markeryness = _ball_histogram_max_markeryness->getDouble();

  //setup values used for the gaussian confidence measurement:
  bool filter_gauss = _ball_gauss_enabled->getBool();
  int exp_area_min =  _ball_gauss_min->getInt();
  int exp_area_max = _ball_gauss_max->getInt();
  double exp_area_var =_ball_gauss_var->getDouble();
  double z_height=_ball_z_height->getDouble();
  const CMVision::Region * reg = 0;

  //acquire orange region list from data-map:
  CMVision::ColorRegionList * colorlist;
  colorlist=(CMVision::ColorRegionList *)data->map.get("cmv_colorlist");
  if (colorlist==0) {
    printf("error in ball detection plugin: no region-lists were found!\n");
    return ProcessingFailed;
  }
  reg = colorlist->getRegionList(color_id_ball).getInitialElement();

  //acquire color-labeled image from data-map:
  const Image<raw8> * image = (Image<raw8> *)(data->map.get("cmv_threshold"));
  if (image==0) {
    printf("error in ball detection plugin: no color-thresholded image was found!\n");
    return ProcessingFailed;
  }


  const CMVision::Region * best_reg = 0;
  float best_conf=0.0;
  if (_max_balls->getInt() > 0) {
    filter.init(reg);
    if (_max_balls->getInt() > 1) printf("Multiple ball detection is currently not supported.\n");
    SSL_DetectionBall* ball = detection_frame->add_balls();
    ball->set_confidence(0.0);
    while((reg = filter.getNext()) != 0 ) {

      float conf = 1.0;

      if (filter_gauss==true) {
        int a = reg->area - bound(reg->area,exp_area_min,exp_area_max);
        conf = gaussian(a / exp_area_var);
      }

      //TODO: add a plugin for confidence masking... possibly multi-layered.
      //      to replace the commented det.mask.get(...) below:
      //if (filter_conf_mask) conf*=det.mask.get(reg->cen_x,reg->cen_y));

      //convert from image to field coordinates:
      vector2d pixel_pos(reg->cen_x,reg->cen_y);
      vector3d field_pos_3d;
      camera_parameters.image2field(field_pos_3d,pixel_pos,z_height);
      vector2d field_pos(field_pos_3d.x,field_pos_3d.y);

      //filter points that are outside of the field:
      if (filter_ball_in_field==true && field_filter.isInFieldOrPlayableBoundary(field_pos)==false) {
        conf = 0.0;
      }

      //filter out points that are deep inside the goal-box
      if (filter_ball_in_goal==true && field_filter.isFarInGoal(field_pos)==true) {
        conf = 0.0;
      }

      //TODO add ball-too-near-robot filter
     /*
        // too-near robot filter
        if(BallTooNearRobotFilter && nb.conf > 0.0){
          double max_sqdist = sq(BallTooNearRobotDist);
          double time_thr = det.timestamp - 0.125;
          double sd = max_sqdist;
          vector2f p = vec2f(nb.loc_z0 + nb.dloc_dz * MaxRobotHeight);
    
          for(int t=1; t<NumTeams; t++){
            sd = det.robots[t].calcNearestRobotDist(p,sd,time_thr);
          }
          if(sd < max_sqdist) nb.conf = 0.0;
        }*/

      // histogram check if enabled
      if(filter_ball_histogram && conf > 0.0 && checkHistogram(image, reg, min_greenness, max_markeryness)==false) {
        conf = 0.0;
      }

      if (conf > best_conf) {
        best_conf = conf;
        best_reg = reg;
      }

    }

    if (best_conf > 0.0 && best_reg !=0) {
      //update result:
      ball->set_confidence(best_conf);

      vector2d pixel_pos(reg->cen_x,reg->cen_y);
      vector3d field_pos_3d;
      camera_parameters.image2field(field_pos_3d,pixel_pos,z_height);

      ball->set_area(best_reg->area);
      ball->set_x(field_pos_3d.x);
      ball->set_y(field_pos_3d.y);
      ball->set_pixel_x(best_reg->cen_x);
      ball->set_pixel_y(best_reg->cen_y);

    }

  }

  return ProcessingOk;

}

