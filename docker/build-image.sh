#!/bin/bash

#  Copyright (C) 2018-2019 LEIDOS.
# 
#  Licensed under the Apache License, Version 2.0 (the "License"); you may not
#  use this file except in compliance with the License. You may obtain a copy of
#  the License at
# 
#  http://www.apache.org/licenses/LICENSE-2.0
# 
#  Unless required by applicable law or agreed to in writing, software
#  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
#  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
#  License for the specific language governing permissions and limitations under
#  the License.

USERNAME=usdotfhwastol
IMAGE=carma

cd "$(dirname "$0")"

echo ""
echo "##### CARMA Docker Image Build Script #####"
echo ""

FULL_VERSION_STRING=$("../engineering_tools/get-carma-version.sh")

echo "Building docker image for CARMA version: $FULL_VERSION_STRING"
echo "Final image name: $USERNAME/$IMAGE:$FULL_VERSION_STRING"

cd ..
docker build -t $USERNAME/$IMAGE:$FULL_VERSION_STRING .
docker tag $USERNAME/$IMAGE:$FULL_VERSION_STRING $USERNAME/$IMAGE:latest

echo "Tagged $USERNAME/$IMAGE:$FULL_VERSION_STRING as $USERNAME/$IMAGE:latest"

echo ""
echo "##### CARMA Docker Image Build Done! #####"

