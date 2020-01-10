/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.apache.arrow.adapter.jdbc.consumer;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.Calendar;
import java.util.Date;

import org.apache.arrow.vector.DateMilliVector;

/**
 * Consumer which consume date type values from {@link ResultSet}.
 * Write the data to {@link org.apache.arrow.vector.DateMilliVector}.
 */
public class DateConsumer {

  /**
   * Creates a consumer for {@link DateMilliVector}.
   */
  public static JdbcConsumer<DateMilliVector> createConsumer(
          DateMilliVector vector, int index, boolean nullable, Calendar calendar) {
    if (nullable) {
      return new NullableDateConsumer(vector, index, calendar);
    } else {
      return new NonNullableDateConsumer(vector, index, calendar);
    }
  }

  /**
   * Nullable consumer for date.
   */
  static class NullableDateConsumer extends BaseConsumer<DateMilliVector> {

    protected final Calendar calendar;

    /**
     * Instantiate a DateConsumer.
     */
    public NullableDateConsumer(DateMilliVector vector, int index) {
      this(vector, index, /* calendar */null);
    }

    /**
     * Instantiate a DateConsumer.
     */
    public NullableDateConsumer(DateMilliVector vector, int index, Calendar calendar) {
      super(vector, index);
      this.calendar = calendar;
    }

    @Override
    public void consume(ResultSet resultSet) throws SQLException {
      Date date = calendar == null ? resultSet.getDate(columnIndexInResultSet) :
          resultSet.getDate(columnIndexInResultSet, calendar);
      if (!resultSet.wasNull()) {
        vector.setSafe(currentIndex, date.getTime());
      }
      currentIndex++;
    }
  }

  /**
   * Non-nullable consumer for date.
   */
  static class NonNullableDateConsumer extends BaseConsumer<DateMilliVector> {

    protected final Calendar calendar;

    /**
     * Instantiate a DateConsumer.
     */
    public NonNullableDateConsumer(DateMilliVector vector, int index) {
      this(vector, index, /* calendar */null);
    }

    /**
     * Instantiate a DateConsumer.
     */
    public NonNullableDateConsumer(DateMilliVector vector, int index, Calendar calendar) {
      super(vector, index);
      this.calendar = calendar;
    }

    @Override
    public void consume(ResultSet resultSet) throws SQLException {
      Date date = calendar == null ? resultSet.getDate(columnIndexInResultSet) :
          resultSet.getDate(columnIndexInResultSet, calendar);
      vector.setSafe(currentIndex, date.getTime());
      currentIndex++;
    }
  }
}


