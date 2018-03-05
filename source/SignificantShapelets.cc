#include "ContingencyTable.hh"
#include "ContingencyTables.hh"
#include "ProgressDisplay.hh"
#include "SignificantShapelets.hh"
#include "SlidingWindow.hh"
#include "Utilities.hh"

// FIXME: remove after debugging or replace with a proper logging
// framework, e.g. Boost.Log.
#include <iostream>
#include <iomanip>

#include <algorithm>
#include <vector>

#include <cassert>
#include <cmath>

#include <boost/math/special_functions/factorials.hpp>

SignificantShapelets::SignificantShapelets( unsigned size, unsigned windowStride )
  : _minWindowSize( size )
  , _maxWindowSize( size )
  , _windowStride( windowStride )
{
}

SignificantShapelets::SignificantShapelets( unsigned minSize, unsigned maxSize, unsigned windowStride )
  : _minWindowSize( minSize )
  , _maxWindowSize( maxSize )
  , _windowStride( windowStride )
{
}

std::vector<SignificantShapelets::SignificantShapelet> SignificantShapelets::operator()(
  const std::vector<TimeSeries>& timeSeries,
  const std::vector<bool>& labels,
  long double& tarone,
  std::vector<long double>& thresholds )
{
  assert( timeSeries.size() == labels.size() );

  unsigned n  = unsigned( timeSeries.size() );
  unsigned n1 = unsigned( std::count( labels.begin(), labels.end(), true ) );

  std::cerr << "n  = " << n << "\n"
            << "n1 = " << n1 << "\n";

  auto min_attainable_p_values = SignificantShapelets::min_attainable_p_values( n, n1 );
  auto windowSizeCorrection    =   boost::math::factorial<long double>( _maxWindowSize )
                                 / boost::math::factorial<long double>( _minWindowSize );

  std::cerr << "* Obtained the following set of minimum attainable $p$-values: ";
  {
    for( auto&& p_val : min_attainable_p_values )
      std::cerr << std::setprecision(32) << p_val << " ";

    std::cerr << "\n";
  }

  std::cerr << "* Window size correction factor: " << windowSizeCorrection << "\n";

  // Remove thresholds that are larger than the desired significance
  // threshold. This does not change testability of patterns because
  // as long as Tarone's threshold is larger than alpha, we are only
  // adding patterns that may never be significant.
  while( min_attainable_p_values.back() > _alpha )
    min_attainable_p_values.pop_back();

  SlidingWindow sw( _minWindowSize,
                    _maxWindowSize,
                    _windowStride );

  sw.setRemoveDuplicates( _removeDuplicates );

  std::vector<TimeSeries> candidates;

  for( std::size_t i = 0; i < timeSeries.size(); i++ )
  {
    auto localCandidates = sw( timeSeries[i] );

    if( not _removeDuplicates )
      candidates.insert( candidates.end(), localCandidates.begin(), localCandidates.end() );
    else
    {
      // If duplicates are to be removed, we are only allowed to copy
      // a local candidate for which no *other* time series satisfies
      // the proxmimity criterion.
      std::copy_if(
        localCandidates.begin(), localCandidates.end(),
        std::back_inserter( candidates ),
        [&candidates] ( const TimeSeries& localCandidate )
        {
          return std::none_of( candidates.begin(), candidates.end(),
            [&localCandidate] ( const TimeSeries& t )
            {
              return t.isClose( localCandidate );
            }
          );
        }
      );
    }
  }

  std::cerr << "* In total: " << candidates.size() << " candidates\n";

  unsigned maxLength = 0;
  for( auto&& ts : timeSeries )
    maxLength = std::max( maxLength, unsigned( ts.length() ) );

  std::cerr << "* Maximum length: " << maxLength << "\n";

  // -------------------------------------------------------------------
  // Main loop for significant shapelet mining
  // -------------------------------------------------------------------
  //
  // Here, we iterate over all remaining candidate shapelets, figure out
  // the best split (with respect to the distance measure), and add them
  // to the list of significant shapelets (at least for the time being).

  // FIXME:
  //   - ignoring predefined distances for now
  //   - make duplicate candidate removal possible

  std::vector<SignificantShapelet> significantShapelets;

  // Initial threshold for Tarone's method. This will be adjusted inside
  // the main loop.
  auto p_tarone = min_attainable_p_values.back();
  thresholds.push_back( p_tarone );

  ProgressDisplay progress( candidates.size() );
  progress.addField( "Testable patterns" );
  progress.addField( "FWER" );
  progress.addField( "Tarone" );
  progress.draw();

  for( std::size_t i = 0; i < candidates.size(); i++ )
  {
    // Set up contingency tables and update them -----------------------
    //
    // This involves iterating over distances in the order in which they
    // appear and constantly update all contingency tables. Pruning will
    // be performed so that not all tables will have to be examined.
    ContingencyTables tables( n, n1 );

    bool skip = false;

    for( std::size_t j = 0; j < timeSeries.size(); j++ )
    {
      tables.insert( candidates[i].distance( timeSeries[j] ), labels[j] );
      tables.prune( p_tarone );

      if( tables.size() == 0 )
      {
        skip = true;
        break;
      }
    }

    ++progress;

    if( skip )
      continue;

    auto pair    = tables.min();
    auto p_value = pair.first;
    auto table   = pair.second;

    // Pattern is testable according to the current threshold set by
    // Tarone's criterion.
    if( p_value <= p_tarone )
    {
      significantShapelets.push_back( {
          candidates[i],
          p_value,
          table
        }
      );

      auto estimateFWER
          = p_tarone
              * static_cast<long double>( significantShapelets.size() )
              * static_cast<long double>( timeSeries.size() )
              * static_cast<long double>( maxLength )
              * static_cast<long double>( windowSizeCorrection );

      // Adjust the testability threshold until the FWER estimate has
      // been sufficiently decreased.
      while( estimateFWER > _alpha )
      {
        min_attainable_p_values.pop_back();
        p_tarone = min_attainable_p_values.back();

        progress.setField( "Tarone", p_tarone );

        significantShapelets.erase(
          std::remove_if( significantShapelets.begin(), significantShapelets.end(),
            [&p_tarone] ( const SignificantShapelet& ss )
            {
              return ss.p > p_tarone;
            }
          ),
          significantShapelets.end()
        );

        estimateFWER
          = p_tarone
              * static_cast<long double>( significantShapelets.size() )
              * static_cast<long double>( timeSeries.size() )
              * static_cast<long double>( maxLength )
              * static_cast<long double>( windowSizeCorrection );

        thresholds.push_back( p_tarone );
      }

      progress.setField( "FWER", estimateFWER );
      progress.setField( "Testable patterns", significantShapelets.size() );
    }
  }

  // Report the lowest threshold according to Tarone. This can be used
  // to decide upon further corrections, such as Bonferroni.
  tarone = p_tarone;

  // Sort by increasing $p$-value in order to make the output easier to
  // parse for humans.
  std::sort( significantShapelets.begin(), significantShapelets.end(),
    [] ( const SignificantShapelet& S, const SignificantShapelet& T )
    {
      return S.p < T.p;
    }
  );

  std::cerr << "* Detected "
            << significantShapelets.size()
            << " significant shapelet"
            << ( significantShapelets.size() != 1 ? "s\n" : "\n" );

  return significantShapelets;
}

std::vector<long double> SignificantShapelets::min_attainable_p_values( unsigned n, unsigned n1 )
{
  ContingencyTable C( n,
                      n1,
                      0.0 ); // use a dummy threshold

  std::vector<long double> p_values;
  p_values.reserve( n );

  // FIXME: we do not have to go over the complete range actually, but
  // it is easier because we adjust the number of objects so that this
  // value is *always* defined.
  for( unsigned rs = 0; rs <= n; rs++ )
    p_values.push_back( C.min_attainable_p(rs) );

  std::sort( p_values.begin(), p_values.end() );

  p_values.erase(
    std::unique( p_values.begin(),
                 p_values.end() ),
    p_values.end()
  );

  return p_values;
}

std::ostream& operator<<( std::ostream& o, const SignificantShapelets::SignificantShapelet& ss )
{
  o << "  {\n"
    << "    \"p_val\": " << std::setprecision( 32 ) << ss.p << ",\n"
    << "    \"table\": [" << ss.table << "],\n"
    << "    \"shapelet\": [\n"
    << "      ";

  for( auto it = ss.shapelet.begin(); it != ss.shapelet.end(); ++it )
  {
    if( it != ss.shapelet.begin() )
      o << ",";

    o << *it;
  }

  o << "    ]\n"
    << "  }";

  return o;
}