# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

module Arrow
  module GenericFilterable
    class << self
      def included(base)
        base.alias_method :filter_raw, :filter
        base.alias_method :filter, :filter_generic
      end
    end

    def filter_generic(filter)
      case filter
      when ::Array
        filter_raw(BooleanArray.new(filter))
      when ChunkedArray
        if respond_to?(:filter_chunked_array)
          filter_chunked_array(filter)
        else
          # TODO: Implement this in C++
          filter_raw(filter.pack)
        end
      else
        filter_raw(filter)
      end
    end
  end
end
