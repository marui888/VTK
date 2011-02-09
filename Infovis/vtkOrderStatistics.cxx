/*=========================================================================

Program:   Visualization Toolkit
Module:    vtkOrderStatistics.cxx

Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
All rights reserved.
See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

This software is distributed WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
/*-------------------------------------------------------------------------
  Copyright 2010 Sandia Corporation.
  Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
  the U.S. Government retains certain rights in this software.
  -------------------------------------------------------------------------*/

#include "vtkToolkits.h"

#include "vtkOrderStatistics.h"
#include "vtkStatisticsAlgorithmPrivate.h"

#include "vtkDoubleArray.h"
#include "vtkIdTypeArray.h"
#include "vtkInformation.h"
#include "vtkIntArray.h"
#include "vtkObjectFactory.h"
#include "vtkMath.h"
#include "vtkMultiBlockDataSet.h"
#include "vtkStringArray.h"
#include "vtkTable.h"
#include "vtkVariantArray.h"

#include <vtksys/stl/vector>
#include <vtksys/stl/map>
#include <vtksys/stl/set>

vtkStandardNewMacro(vtkOrderStatistics);

// ----------------------------------------------------------------------
vtkOrderStatistics::vtkOrderStatistics()
{
  this->QuantileDefinition = vtkOrderStatistics::InverseCDFAveragedSteps;
  this->NumberOfIntervals = 4; // By default, calculate 5-points statistics

  this->AssessNames->SetNumberOfValues( 1 );
  this->AssessNames->SetValue( 0, "Quantile" );
}

// ----------------------------------------------------------------------
vtkOrderStatistics::~vtkOrderStatistics()
{
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::PrintSelf( ostream &os, vtkIndent indent )
{
  this->Superclass::PrintSelf( os, indent );
  os << indent << "NumberOfIntervals: " << this->NumberOfIntervals << endl;
  os << indent << "QuantileDefinition: " << this->QuantileDefinition << endl;
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::SetQuantileDefinition( int qd )
{
  switch ( qd )
    {
    case vtkOrderStatistics::InverseCDF:
      break;
    case vtkOrderStatistics::InverseCDFAveragedSteps:
      break;
    default:
      vtkWarningMacro( "Incorrect type of quantile definition: "
                       <<qd
                       <<". Ignoring it." );
      return;
    }

  this->QuantileDefinition =  static_cast<vtkOrderStatistics::QuantileDefinitionType>( qd );
  this->Modified();

  return;
}

// ----------------------------------------------------------------------
bool vtkOrderStatistics::SetParameter( const char* parameter,
                                       int vtkNotUsed( index ),
                                       vtkVariant value )
{
  if ( ! strcmp( parameter, "NumberOfIntervals" ) )
    {
    this->SetNumberOfIntervals( value.ToInt() );

    return true;
    }

  if ( ! strcmp( parameter, "QuantileDefinition" ) )
    {
    this->SetQuantileDefinition( value.ToInt() );

    return true;
    }

  return false;
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::Learn( vtkTable* inData,
                                vtkTable* vtkNotUsed( inParameters ),
                                vtkMultiBlockDataSet* outMeta )
{
  if ( ! inData )
    {
    return;
    }

  if ( ! outMeta )
    {
    return;
    }

  // Loop over requests
  vtkIdType nRow = inData->GetNumberOfRows();
  for ( vtksys_stl::set<vtksys_stl::set<vtkStdString> >::iterator rit = this->Internals->Requests.begin();
        rit != this->Internals->Requests.end(); ++ rit )
    {
    // Each request contains only one column of interest (if there are others, they are ignored)
    vtksys_stl::set<vtkStdString>::const_iterator it = rit->begin();
    vtkStdString col = *it;
    if ( ! inData->GetColumnByName( col ) )
      {
      vtkWarningMacro( "InData table does not have a column "
                       << col.c_str()
                       << ". Ignoring it." );
      continue;
      }

    // Get hold of data for this variable
    vtkAbstractArray* vals = inData->GetColumnByName( col );

    // Create histogram table for this variable
    vtkTable* histogramTab = vtkTable::New();

    // Row to be used to insert into histogram table
    vtkVariantArray* row = vtkVariantArray::New();
    row->SetNumberOfValues( 2 );

    // Switch depending on data type
    if ( vals->IsA("vtkDataArray") )
      {
      vtkDoubleArray* doubleCol = vtkDoubleArray::New();
      doubleCol->SetName( "Value" );
      histogramTab->AddColumn( doubleCol );
      doubleCol->Delete();

      // Value of cardinality row is NaN
      double noVal = vtkMath::Nan();
      row->SetValue( 0, noVal );
      }
    else if ( vals->IsA("vtkStringArray") )
      {
      vtkStringArray* stringCol = vtkStringArray::New();
      stringCol->SetName( "Value" );
      histogramTab->AddColumn( stringCol );
      stringCol->Delete();

      // Value of cardinality row is the empty string
      vtkStdString noVal = vtkStdString( "" );
      row->SetValue( 0, noVal );
      }
    else if ( vals->IsA("vtkVariantArray") )
      {
      vtkVariantArray* variantCol = vtkVariantArray::New();
      variantCol->SetName( "Value" );
      histogramTab->AddColumn( variantCol );
      variantCol->Delete();

      // Value of cardinality row is the empty variant
      vtkVariant noVal = vtkVariant( "" );
      row->SetValue( 0, noVal );
      }
    else
      {
      vtkWarningMacro( "Unsupported data type for column "
                       << col.c_str()
                       << ". Ignoring it." );

      continue;
      }

    vtkIdTypeArray* idTypeCol = vtkIdTypeArray::New();
    idTypeCol->SetName( "Cardinality" );
    histogramTab->AddColumn( idTypeCol );
    idTypeCol->Delete();

    // Insert first row which will always contain the data set cardinality
    // NB: The cardinality is calculated in derive mode ONLY, and is set to an invalid value of -1 in
    // learn mode to make it clear that it is not a correct value. This is an issue of database
    // normalization: including the cardinality to the other counts can lead to inconsistency, in particular
    // when the input meta table is calculated by something else than the learn mode (e.g., is specified
    // by the user).
    row->SetValue( 1, -1 );
    histogramTab->InsertNextRow( row );

    // Switch depending on data type
    if ( vals->IsA("vtkDataArray") )
      {
      // Downcast column to double array for efficient data access
      vtkDataArray* dvals = vtkDataArray::SafeDownCast( vals );

      // Calculate histogram
      vtksys_stl::map<double,vtkIdType> histogram;
      for ( vtkIdType r = 0; r < nRow; ++ r )
        {
        ++ histogram[dvals->GetTuple1( r )];
        }

      // Store histogram
      for ( vtksys_stl::map<double,vtkIdType>::iterator mit = histogram.begin();
            mit != histogram.end(); ++ mit  )
        {
        row->SetValue( 0, mit->first );
        row->SetValue( 1, mit->second );
        histogramTab->InsertNextRow( row );
        }
      } // if ( vals->IsA("vtkDataArray") )
    else if ( vals->IsA("vtkStringArray") )
      {
      // Downcast column to string array for efficient data access
      vtkStringArray* svals = vtkStringArray::SafeDownCast( vals );

      // Calculate histogram
      vtksys_stl::map<vtkStdString,vtkIdType> histogram;
      for ( vtkIdType r = 0; r < nRow; ++ r )
        {
        ++ histogram[svals->GetValue( r )];
        }

      // Store histogram
      for ( vtksys_stl::map<vtkStdString,vtkIdType>::iterator mit = histogram.begin();
            mit != histogram.end(); ++ mit  )
        {
        row->SetValue( 0, mit->first );
        row->SetValue( 1, mit->second );
        histogramTab->InsertNextRow( row );
        }
      } // else if ( vals->IsA("vtkStringArray") )
    else if ( vals->IsA("vtkVariantArray") )
      {
      // Downcast column to string array for efficient data access
      vtkVariantArray* vvals = vtkVariantArray::SafeDownCast( vals );

      // Calculate histogram
      vtksys_stl::map<vtkVariant,vtkIdType> histogram;
      for ( vtkIdType r = 0; r < nRow; ++ r )
        {
        ++ histogram[vvals->GetVariantValue( r )];
        }

      // Store histogram
      for ( vtksys_stl::map<vtkVariant,vtkIdType>::iterator mit = histogram.begin();
            mit != histogram.end(); ++ mit  )
        {
        row->SetValue( 0, mit->first );
        row->SetValue( 1, mit->second );
        histogramTab->InsertNextRow( row );
        }
      } // else if ( vals->IsA("vtkVariantArray") )
    else
      {
      vtkWarningMacro( "Unsupported data type for column "
                       << col.c_str()
                       << ". Ignoring it." );

      continue;
      } // else

    // Resize output meta so histogram table can be appended
    unsigned int nBlocks = outMeta->GetNumberOfBlocks();
    outMeta->SetNumberOfBlocks( nBlocks + 1 );
    outMeta->GetMetaData( static_cast<unsigned>( nBlocks ) )->Set( vtkCompositeDataSet::NAME(), col );
    outMeta->SetBlock( nBlocks, histogramTab );

    // Clean up
    histogramTab->Delete();
    row->Delete();
    } // rit

  return;
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::Derive( vtkMultiBlockDataSet* inMeta )
{
  if ( ! inMeta || inMeta->GetNumberOfBlocks() < 1 )
    {
    return;
    }

  // Create quantiles table
  vtkTable* quantileTab = vtkTable::New();

  vtkStringArray* stringCol = vtkStringArray::New();
  stringCol->SetName( "Quantile" );
  quantileTab->AddColumn( stringCol );
  stringCol->Delete();

  double dq = 1. / static_cast<double>( this->NumberOfIntervals );
  for ( int i = 0; i <= this->NumberOfIntervals; ++ i )
    {

    // Handle special case of quartiles and median for convenience
    div_t q = div( i << 2, this->NumberOfIntervals );
    if ( q.rem )
      {
      // General case
      stringCol->InsertNextValue( vtkStdString( vtkVariant( i * dq ).ToString() + "-quantile" ).c_str() );
      }
    else
      {
      // Case where q is a multiple of 4
      switch ( q.quot )
        {
        case 0:
          stringCol->InsertNextValue( "Minimum" );
          break;
        case 1:
          stringCol->InsertNextValue( "First Quartile" );
          break;
        case 2:
          stringCol->InsertNextValue( "Median" );
          break;
        case 3:
          stringCol->InsertNextValue( "Third Quartile" );
          break;
        case 4:
          stringCol->InsertNextValue( "Maximum" );
          break;
        default:
          stringCol->InsertNextValue( vtkStdString( vtkVariant( i * dq ).ToString() + "-quantile" ).c_str() );
          break;
        }
      }
    }

  // Iterate over primary tables
  unsigned int nBlocks = inMeta->GetNumberOfBlocks();
  for ( unsigned int b = 0; b < nBlocks; ++ b )
    {
    vtkTable* histogramTab = vtkTable::SafeDownCast( inMeta->GetBlock( b ) );
    if ( ! histogramTab  )
      {
      continue;
      }

    // Downcast columns to typed arrays for efficient data access
    vtkAbstractArray* vals = vtkAbstractArray::SafeDownCast( histogramTab->GetColumnByName( "Value" ) );
    vtkIdTypeArray* card = vtkIdTypeArray::SafeDownCast( histogramTab->GetColumnByName( "Cardinality" ) );

    // The CDF will be used for quantiles calculation (effectively as a reverse look-up table
    // NB: first entry (index 0) is not used
    vtkIdType nRowHist = histogramTab->GetNumberOfRows();
    vtkIdType* cdf =  new vtkIdType[nRowHist];

    // Calculate variable cardinality and CDF
    vtkIdType c;
    vtkIdType n = 0;
    for ( int r = 1; r < nRowHist; ++ r ) // Skip first row where data set cardinality will be stored
      {
      // Update cardinality and CDF
      c = card->GetValue( r );
      n += c;
      cdf[r] = n;
      }

    // Store cardinality
    histogramTab->SetValueByName( 0, "Cardinality", n );

    // Find or create column of probability mass function of histogram table
    vtkStdString probaName( "P" );
    vtkDoubleArray* probaCol;
    vtkAbstractArray* abstrCol = histogramTab->GetColumnByName( probaName );
    if ( ! abstrCol )
      {
      probaCol = vtkDoubleArray::New();
      probaCol->SetName( probaName );
      probaCol->SetNumberOfTuples( nRowHist );
      histogramTab->AddColumn( probaCol );
      probaCol->Delete();
      }
    else
      {
      probaCol = vtkDoubleArray::SafeDownCast( abstrCol );
      }

    // Store invalid probability for cardinality row
    histogramTab->SetValueByName( 0, "P", -1. );

    // Finally calculate and store probabilities
    double inv_n = 1. / n;
    double p;
    for ( int r = 1; r < nRowHist; ++ r ) // Skip first row which contains data set cardinality
      {
      c = card->GetValue( r );
      p = inv_n * c;

      probaCol->SetValue( r, p );
      }

    // Storage for quantile indices
    vtksys_stl::vector<vtksys_stl::pair<vtkIdType,vtkIdType> > quantileIndices;
    vtksys_stl::pair<vtkIdType,vtkIdType> qIdxPair;

    // First quantile index is always 1 with no jump (corresponding to the first and the smallest value)
    qIdxPair.first = 1;
    qIdxPair.second = 1;
    quantileIndices.push_back( qIdxPair );

    // Calculate all interior quantiles (i.e. for 0 < k < q)
    vtkIdType rank = 1;
    double dh = n / static_cast<double>( this->NumberOfIntervals );
    for ( vtkIdType k = 1; k < this->NumberOfIntervals; ++ k )
      {
      // Calculate np value
      double np = k * dh;

      // Calculate first quantile index
      vtkIdType qIdx1;
      if ( this->QuantileDefinition == vtkOrderStatistics::InverseCDFAveragedSteps )
        {
        qIdx1 = static_cast<vtkIdType>( round( np ) );
        }
      else
        {
        qIdx1 = static_cast<vtkIdType>( ceil( np ) );
        }

      // Find rank of the entry where first quantile index is reached using the CDF
      while ( qIdx1 > cdf[rank] )
        {
        ++ rank;

        if ( rank > nRowHist )
          {
          vtkErrorMacro( "Inconsistent quantile table: at last rank "
                         << rank
                         << " the CDF is  "
                         << cdf[rank]
                         <<" < "
                         << qIdx1
                         << " the quantile index. Cannot derive model." );
          return;
          }
        }

      // Store rank in histogram of first quantile index
      qIdxPair.first = rank;

      // Decide whether midpoint interpolation will be used for this numeric type input
      if ( this->QuantileDefinition == vtkOrderStatistics::InverseCDFAveragedSteps )
        {
        // Calculate second quantile index for mid-point interpolation
        vtkIdType qIdx2 = static_cast<vtkIdType>( floor( np + 1. ) );

        // If the two quantile indices differ find rank where second is reached
        if ( qIdx1 != qIdx2 )
          {
          while ( qIdx2 > cdf[rank] )
            {
            ++ rank;

            if ( rank > nRowHist )
              {
              vtkErrorMacro( "Inconsistent quantile table: at last rank "
                             << rank
                             << " the CDF is  "
                             << cdf[rank]
                             <<" < "
                             << qIdx2
                             << " the quantile index. Cannot derive model." );
              return;
              }
            }
          }
        } // if ( this->QuantileDefinition == vtkOrderStatistics::InverseCDFAveragedSteps )

      // Store rank in histogram of second quantile index
      qIdxPair.second = rank;

      // Store pair of ranks
      quantileIndices.push_back( qIdxPair );
      }

    // Last quantile index is always cardinality with no jump (corresponding to the last and thus largest value)
    qIdxPair.first = nRowHist - 1;
    qIdxPair.second = nRowHist - 1;;
    quantileIndices.push_back( qIdxPair );

    // Finally prepare quantile values column
    vtkStdString varName = inMeta->GetMetaData( b )->Get( vtkCompositeDataSet::NAME() );

    // Switch depending on data type
    if ( vals->IsA("vtkDataArray") )
      {
      // Downcast column to double array for efficient data access
      vtkDataArray* dvals = vtkDataArray::SafeDownCast( vals );

      // Create column for quantiles of the same type as the values
      vtkDataArray* quantCol = vtkDataArray::CreateDataArray( dvals->GetDataType() );
      quantCol->SetName( varName );
      quantCol->SetNumberOfTuples( this->NumberOfIntervals + 1 );
      quantileTab->AddColumn( quantCol );
      quantCol->Delete();

      // Decide whether midpoint interpolation will be used for this numeric type input
      if ( this->QuantileDefinition == vtkOrderStatistics::InverseCDFAveragedSteps )
        {
        // Compute and store quantile values
        vtkIdType k = 0;
        for ( vtksys_stl::vector<vtksys_stl::pair<vtkIdType,vtkIdType> >::iterator qit = quantileIndices.begin();
              qit != quantileIndices.end(); ++ qit, ++ k )
          {
          // Retrieve data values from rank into histogram and interpolate
          double Qp = .5 * ( dvals->GetTuple1( qit->first )
                             + dvals->GetTuple1( qit->second ) );

          // Store quantile value
          quantCol->SetTuple1( k, Qp );
          } // qit
        }
      else // if ( this->QuantileDefinition == vtkOrderStatistics::InverseCDFAveragedSteps )
        {
        // Compute and store quantile values
        vtkIdType k = 0;
        for ( vtksys_stl::vector<vtksys_stl::pair<vtkIdType,vtkIdType> >::iterator qit = quantileIndices.begin();
              qit != quantileIndices.end(); ++ qit, ++ k )
          {
          // Retrieve data value from rank into histogram
          double Qp = dvals->GetTuple1( qit->first );

          // Store quantile value
          quantCol->SetTuple1( k, Qp );
          } // qit
        } // else ( this->QuantileDefinition == vtkOrderStatistics::InverseCDFAveragedSteps )
      } // if ( vals->IsA("vtkDataArray") )
    else if ( vals->IsA("vtkStringArray") )
      {
      // Downcast column to string array for efficient data access
      vtkStringArray* svals = vtkStringArray::SafeDownCast( vals );

      // Create column for quantiles of the same type as the values
      vtkStringArray* quantCol = vtkStringArray::New();
      quantCol->SetName( varName );
      quantCol->SetNumberOfTuples( this->NumberOfIntervals + 1 );
      quantileTab->AddColumn( quantCol );
      quantCol->Delete();

      // Compute and store quantile values
      vtkIdType k = 0;
      for ( vtksys_stl::vector<vtksys_stl::pair<vtkIdType,vtkIdType> >::iterator qit = quantileIndices.begin();
            qit != quantileIndices.end(); ++ qit, ++ k )
        {
        // Retrieve data value from rank into histogram
        vtkStdString Qp = svals->GetValue( qit->first );

        // Store quantile value
        quantCol->SetValue( k, Qp );
        }
      } // else if ( vals->IsA("vtkStringArray") )
    else if ( vals->IsA("vtkVariantArray") )
      {
      // Downcast column to variant array for efficient data access
      vtkVariantArray* vvals = vtkVariantArray::SafeDownCast( vals );

      // Create column for quantiles of the same type as the values
      vtkVariantArray* quantCol = vtkVariantArray::New();
      quantCol->SetName( varName );
      quantCol->SetNumberOfTuples( this->NumberOfIntervals + 1 );
      quantileTab->AddColumn( quantCol );
      quantCol->Delete();

      // Compute and store quantile values
      vtkIdType k = 0;
      for ( vtksys_stl::vector<vtksys_stl::pair<vtkIdType,vtkIdType> >::iterator qit = quantileIndices.begin();
            qit != quantileIndices.end(); ++ qit, ++ k )
        {
        // Retrieve data value from rank into histogram
        vtkVariant Qp = vvals->GetValue( qit->first );

        // Store quantile value
        quantCol->SetValue( k, Qp );
        }
      } // else if ( vals->IsA("vtkVariantArray") )
    else
      {
      vtkWarningMacro( "Unsupported data type for column "
                       << varName.c_str()
                       << ". Cannot calculate quantiles for it." );

      continue;
      } // else

    // Clean up
    delete [] cdf;
    } // for ( unsigned int b = 0; b < nBlocks; ++ b )

  // Resize output meta so quantile table can be appended
  nBlocks = inMeta->GetNumberOfBlocks();
  inMeta->SetNumberOfBlocks( nBlocks + 1 );
  inMeta->GetMetaData( static_cast<unsigned>( nBlocks ) )->Set( vtkCompositeDataSet::NAME(), "Quantiles" );
  inMeta->SetBlock( nBlocks, quantileTab );

  // Clean up
  quantileTab->Delete();
}

// ----------------------------------------------------------------------
void vtkOrderStatistics::Test( vtkTable* inData,
                               vtkMultiBlockDataSet* inMeta,
                               vtkTable* outMeta )
{
  if ( ! inMeta )
    {
    return;
    }

  unsigned nBlocks = inMeta->GetNumberOfBlocks();
  if ( nBlocks < 1 )
    {
    return;
    }

  vtkTable* quantileTab = vtkTable::SafeDownCast( inMeta->GetBlock( nBlocks - 1 ) );
  if ( ! quantileTab
       || inMeta->GetMetaData( nBlocks - 1 )->Get( vtkCompositeDataSet::NAME() ) != vtkStdString( "Quantiles" ) )
    {
    return;
    }

  if ( ! outMeta )
    {
    return;
    }

  // Prepare columns for the test:
  // 0: variable name
  // 1: Maximum vertical distance between CDFs
  // 2: Kolmogorov-Smirnov test statistic (the above times the square root of the cardinality)
  // NB: These are not added to the output table yet, for they will be filled individually first
  //     in order that R be invoked only once.
  vtkStringArray* nameCol = vtkStringArray::New();
  nameCol->SetName( "Variable" );

  vtkDoubleArray* distCol = vtkDoubleArray::New();
  distCol->SetName( "Maximum Distance" );

  vtkDoubleArray* statCol = vtkDoubleArray::New();
  statCol->SetName( "Kolmogorov-Smirnov" );

  // Prepare storage for quantiles and model CDFs
  vtkIdType nQuant = quantileTab->GetNumberOfRows();
  vtkStdString* quantiles = new vtkStdString[nQuant];

  // Loop over requests
  vtkIdType nRowData = inData->GetNumberOfRows();
  double inv_nq =  1. / nQuant;
  double inv_card = 1. / nRowData;
  double sqrt_card = sqrt( static_cast<double>( nRowData ) );
  for ( vtksys_stl::set<vtksys_stl::set<vtkStdString> >::const_iterator rit = this->Internals->Requests.begin();
        rit != this->Internals->Requests.end(); ++ rit )
    {
    // Each request contains only one column of interest (if there are others, they are ignored)
    vtksys_stl::set<vtkStdString>::const_iterator it = rit->begin();
    vtkStdString varName = *it;
    if ( ! inData->GetColumnByName( varName ) )
      {
      vtkWarningMacro( "InData table does not have a column "
                       << varName.c_str()
                       << ". Ignoring it." );
      continue;
      }

    // Find the quantile column that corresponds to the variable of the request
    vtkAbstractArray* quantCol = quantileTab->GetColumnByName( varName );
    if ( ! quantCol )
      {
      vtkWarningMacro( "Quantile table table does not have a column "
                       << varName.c_str()
                       << ". Ignoring it." );
      continue;
      }

    // First iterate over all observations to calculate empirical PDF
    typedef vtksys_stl::map<vtkStdString,double> CDF;
    CDF cdfEmpirical;
    for ( vtkIdType j = 0; j < nRowData; ++ j )
      {
      // Read observation and update PDF
      cdfEmpirical
        [inData->GetValueByName( j, varName ).ToString()] += inv_card;
      }

    // Now integrate to obtain empirical CDF
    double sum = 0.;
    for ( CDF::iterator cit = cdfEmpirical.begin(); cit != cdfEmpirical.end(); ++ cit )
      {
      sum += cit->second;
      cit->second = sum;
      }

    // Sanity check: verify that empirical CDF = 1
    if ( fabs( sum - 1. ) > 1.e-6 )
      {
      vtkWarningMacro( "Incorrect empirical CDF for variable:"
                       << varName.c_str()
                       << ". Ignoring it." );

      continue;
      }

    // Retrieve quantiles to calculate model CDF and insert value into empirical CDF
    for ( vtkIdType i = 0; i < nQuant; ++ i )
      {
      // Read quantile and update CDF
      quantiles[i] = quantileTab->GetValueByName( i, varName ).ToString();

      // Update empirical CDF if new value found (with unknown ECDF)
      vtksys_stl::pair<CDF::iterator,bool> result
        = cdfEmpirical.insert( vtksys_stl::pair<vtkStdString,double>( quantiles[i], -1 ) );
      if ( result.second == true )
        {
        CDF::iterator eit = result.first;
        // Check if new value has no predecessor, in which case CDF = 0
        if ( eit ==  cdfEmpirical.begin() )
          {
          result.first->second = 0.;
          }
        else
          {
          -- eit;
          result.first->second = eit->second;
          }
        }
      }

    // Iterate over all CDF jump values
    int currentQ = 0;
    double mcdf = 0.;
    double Dmn = 0.;
    for ( CDF::iterator cit = cdfEmpirical.begin(); cit != cdfEmpirical.end(); ++ cit )
      {
      // If observation is smaller than minimum then there is nothing to do
      if ( cit->first >= quantiles[0] )
        {
        while ( currentQ < nQuant && cit->first >= quantiles[currentQ] )
          {
          ++ currentQ;
          }

        // Calculate model CDF at observation
        mcdf = currentQ * inv_nq;
        }

      // Calculate vertical distance between CDFs and update maximum if needed
      double d = fabs( cit->second - mcdf );
      if ( d > Dmn )
        {
        Dmn =  d;
        }
      }

    // Insert variable name and calculated Kolmogorov-Smirnov statistic
    // NB: R will be invoked only once at the end for efficiency
    nameCol->InsertNextValue( varName );
    distCol->InsertNextTuple1( Dmn );
    statCol->InsertNextTuple1( sqrt_card * Dmn );
    } // rit

  // Now, add the already prepared columns to the output table
  outMeta->AddColumn( nameCol );
  outMeta->AddColumn( distCol );
  outMeta->AddColumn( statCol );

  // Clean up
  delete [] quantiles;
  nameCol->Delete();
  distCol->Delete();
  statCol->Delete();
}

// ----------------------------------------------------------------------
class DataArrayQuantizer : public vtkStatisticsAlgorithm::AssessFunctor
{
public:
  vtkDataArray* Data;
  vtkDataArray* Quantiles;

  DataArrayQuantizer( vtkAbstractArray* vals,
                      vtkAbstractArray* quantiles )
  {
    this->Data      = vtkDataArray::SafeDownCast( vals );
    this->Quantiles = vtkDataArray::SafeDownCast( quantiles );
  }
  virtual ~DataArrayQuantizer()
  {
  }
  virtual void operator() ( vtkVariantArray* result,
                            vtkIdType id )
  {
    result->SetNumberOfValues( 1 );

    double dval = this->Data->GetTuple1( id );
    if ( dval < this->Quantiles->GetTuple1( 0 ) )
      {
      // dval is smaller than lower bound
      result->SetValue( 0, 0 );

      return;
      }

    vtkIdType q = 1;
    vtkIdType n = this->Quantiles->GetNumberOfTuples();
    while ( q < n && dval > this->Quantiles->GetTuple1( q ) )
      {
      ++ q;
      }

    result->SetValue( 0, q );
  }
};

// ----------------------------------------------------------------------
class StringArrayQuantizer : public vtkStatisticsAlgorithm::AssessFunctor
{
public:
  vtkStringArray* Data;
  vtkStringArray* Quantiles;

  StringArrayQuantizer( vtkAbstractArray* vals,
                       vtkAbstractArray* quantiles )
  {
    this->Data      = vtkStringArray::SafeDownCast( vals );
    this->Quantiles = vtkStringArray::SafeDownCast( quantiles );
  }
  virtual ~StringArrayQuantizer()
  {
  }
  virtual void operator() ( vtkVariantArray* result,
                            vtkIdType id )
  {
    result->SetNumberOfValues( 1 );

    vtkStdString sval = this->Data->GetValue( id );
    if ( sval < this->Quantiles->GetValue( 0 ) )
      {
      // sval is smaller than lower bound
      result->SetValue( 0, 0 );

      return;
      }

    vtkIdType q = 1;
    vtkIdType n = this->Quantiles->GetNumberOfValues();
    while ( q < n && sval > this->Quantiles->GetValue( q ) )
      {
      ++ q;
      }

    result->SetValue( 0, q );
  }
};

// ----------------------------------------------------------------------
class VariantArrayQuantizer : public vtkStatisticsAlgorithm::AssessFunctor
{
public:
  vtkVariantArray* Data;
  vtkVariantArray* Quantiles;

  VariantArrayQuantizer( vtkAbstractArray* vals,
                         vtkAbstractArray* quantiles )
  {
    this->Data      = vtkVariantArray::SafeDownCast( vals );
    this->Quantiles = vtkVariantArray::SafeDownCast( quantiles );
  }
  virtual ~VariantArrayQuantizer()
  {
  }
  virtual void operator() ( vtkVariantArray* result,
                            vtkIdType id )
  {
    result->SetNumberOfValues( 1 );

    vtkVariant vval = this->Data->GetValue( id );
    if ( vval < this->Quantiles->GetValue( 0 ) )
      {
      // vval is smaller than lower bound
      result->SetValue( 0, 0 );

      return;
      }

    vtkIdType q = 1;
    vtkIdType n = this->Quantiles->GetNumberOfValues();
    while ( q < n && vval > this->Quantiles->GetValue( q ) )
      {
      ++ q;
      }

    result->SetValue( 0, q );
  }
};

// ----------------------------------------------------------------------
void vtkOrderStatistics::SelectAssessFunctor( vtkTable* outData,
                                              vtkDataObject* inMetaDO,
                                              vtkStringArray* rowNames,
                                              AssessFunctor*& dfunc )
{
  dfunc = 0;
  vtkMultiBlockDataSet* inMeta = vtkMultiBlockDataSet::SafeDownCast( inMetaDO );
  if ( ! inMeta )
    {
    return;
    }

  unsigned nBlocks = inMeta->GetNumberOfBlocks();
  if ( nBlocks < 1 )
    {
    return;
    }

  vtkTable* quantileTab = vtkTable::SafeDownCast( inMeta->GetBlock( nBlocks - 1 ) );
  if ( ! quantileTab
       || inMeta->GetMetaData( nBlocks - 1 )->Get( vtkCompositeDataSet::NAME() ) != vtkStdString( "Quantiles" ) )
    {
    return;
    }

  // Retrieve name of variable of the request
  vtkStdString varName = rowNames->GetValue( 0 );

  // Grab the data for the requested variable
  vtkAbstractArray* vals = outData->GetColumnByName( varName );
  if ( ! vals )
    {
    return;
    }

  // Find the quantile column that corresponds to the variable of the request
  vtkAbstractArray* quantiles = quantileTab->GetColumnByName( varName );
  if ( ! quantiles )
    {
    vtkWarningMacro( "Quantile table table does not have a column "
                     << varName.c_str()
                     << ". Ignoring it." );
    return;
    }

  // Select assess functor depending on data and quantile type
  if ( vals->IsA("vtkDataArray") && quantiles->IsA("vtkDataArray") )
    {
    dfunc = new DataArrayQuantizer( vals, quantiles );
    }
  else if ( vals->IsA("vtkStringArray") && quantiles->IsA("vtkStringArray") )
    {
    dfunc = new StringArrayQuantizer( vals, quantiles );
    }
  else if ( vals->IsA("vtkVariantArray") && quantiles->IsA("vtkVariantArray") )
    {
    dfunc = new VariantArrayQuantizer( vals, quantiles );
    }
  else
    {
    vtkWarningMacro( "Unsupported (data,quantiles) type for column "
                     << varName.c_str()
                     << ": data type is "
                     << vals->GetClassName()
                     << " and quantiles type is "
                     << quantiles->GetClassName()
                     << ". Ignoring it." );
    }
}
