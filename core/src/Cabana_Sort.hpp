/****************************************************************************
 * Copyright (c) 2018-2019 by the Cabana authors                            *
 * All rights reserved.                                                     *
 *                                                                          *
 * This file is part of the Cabana library. Cabana is distributed under a   *
 * BSD 3-clause license. For the licensing terms see the LICENSE file in    *
 * the top-level directory.                                                 *
 *                                                                          *
 * SPDX-License-Identifier: BSD-3-Clause                                    *
 ****************************************************************************/

#ifndef CABANA_SORT_HPP
#define CABANA_SORT_HPP

#include <Cabana_AoSoA.hpp>
#include <Cabana_Slice.hpp>
#include <Cabana_DeepCopy.hpp>

#include <Kokkos_Core.hpp>
#include <Kokkos_Sort.hpp>

#include <type_traits>

namespace Cabana
{
//---------------------------------------------------------------------------//
/*!
  \class BinningData
  \brief Data describing the bin sizes and offsets resulting from a binning
  operation.
*/
template<class MemorySpace>
class BinningData
{
  public:

    using memory_space = MemorySpace;
    using execution_space = typename memory_space::execution_space;
    using size_type = typename memory_space::size_type;
    using CountView = Kokkos::View<const int*,memory_space>;
    using OffsetView = Kokkos::View<size_type*,memory_space>;

    BinningData()
        : _nbin(0)
    {}

    BinningData( const std::size_t begin,
                 const std::size_t end,
                 CountView counts,
                 OffsetView offsets,
                 OffsetView permute_vector )
        : _begin( begin )
        , _end( end )
        , _nbin( counts.extent(0) )
        , _counts( counts )
        , _offsets( offsets )
        , _permute_vector( permute_vector )
    {}

    /*!
      \brief Get the number of bins.
      \return The number of bins.
    */
    KOKKOS_INLINE_FUNCTION
    int numBin() const
    { return _nbin; }

    /*!
      \brief Given a bin get the number of tuples it contains.
      \param bin_id The bin id.
      \return The number of tuples in the bin.
    */
    KOKKOS_INLINE_FUNCTION
    int binSize( const size_type bin_id ) const
    { return _counts( bin_id ); }

    /*!
      \brief Given a bin get the tuple index at which it sorts.
      \param bin_id The bin id.
      \return The starting tuple index of the bin.
    */
    KOKKOS_INLINE_FUNCTION
    size_type binOffset( const size_type bin_id ) const
    { return _offsets( bin_id ); }

    /*!
      \brief Given a local tuple id in the binned layout, get the id of the
      tuple in the old (unbinned) layout.
    */
    KOKKOS_INLINE_FUNCTION
    size_type permutation( const size_type tuple_id ) const
    { return _permute_vector(tuple_id); }

    /*!
      \brief The beginning tuple index in the binning.
    */
    KOKKOS_INLINE_FUNCTION
    std::size_t rangeBegin() const
    { return _begin; }

    /*!
      \brief The ending tuple index in the binning.
    */
    KOKKOS_INLINE_FUNCTION
    std::size_t rangeEnd() const
    { return _end; }

  private:

    std::size_t _begin;
    std::size_t _end;
    int _nbin;
    CountView _counts;
    OffsetView _offsets;
    OffsetView _permute_vector;
};

//---------------------------------------------------------------------------//
// Static type checker.
template<typename >
struct is_binning_data : public std::false_type {};

template<typename MemorySpace>
struct is_binning_data<BinningData<MemorySpace> >
    : public std::true_type {};

template<typename MemorySpace>
struct is_binning_data<const BinningData<MemorySpace> >
    : public std::true_type {};

namespace Impl
{
//---------------------------------------------------------------------------//
// Create a permutation vector over a range subset using a comparator over the
// given Kokkos View of keys.
template<class KeyViewType, class Comparator>
BinningData<typename KeyViewType::memory_space>
kokkosBinSort( KeyViewType keys,
               Comparator comp,
               const bool sort_within_bins,
               const std::size_t begin,
               const std::size_t end )
{
    Kokkos::BinSort<KeyViewType,Comparator> bin_sort(
        keys, begin, end, comp, sort_within_bins );
    bin_sort.create_permute_vector();
    return BinningData<typename KeyViewType::memory_space>(
            begin,
            end,
            bin_sort.get_bin_count(),
            bin_sort.get_bin_offsets(),
            bin_sort.get_permute_vector() );
}

//---------------------------------------------------------------------------//
// Given a set of keys, find the minimum and maximum over the given range.
template<class KeyViewType>
Kokkos::MinMaxScalar<typename KeyViewType::non_const_value_type>
keyMinMax( KeyViewType keys, const std::size_t begin, const std::size_t end )
{
    Kokkos::MinMaxScalar<typename KeyViewType::non_const_value_type> result;
    Kokkos::MinMax<typename KeyViewType::non_const_value_type> reducer(result);
    Kokkos::parallel_reduce(
        "Cabana::keyMinMax",
        Kokkos::RangePolicy<typename KeyViewType::execution_space>(begin,end),
        Kokkos::Impl::min_max_functor<KeyViewType>(keys),
        reducer );
    Kokkos::fence();
    return result;
}

//---------------------------------------------------------------------------//
// Sort an AoSoA over a subset of its range using the given Kokkos View of
// keys.
template<class KeyViewType>
BinningData<typename KeyViewType::memory_space>
kokkosBinSort1d( KeyViewType keys,
                 const int nbin,
                 const bool sort_within_bins,
                 const std::size_t begin,
                 const std::size_t end )
{
    // Find the minimum and maximum key values.
    auto key_bounds = Impl::keyMinMax( keys, begin, end );

    // Create a sorting comparator.
    Kokkos::BinOp1D<KeyViewType> comp(
        nbin, key_bounds.min_val, key_bounds.max_val );

    // BinSort
    return kokkosBinSort( keys, comp, sort_within_bins, begin, end );
}

//---------------------------------------------------------------------------//
// Copy the a 1D slice into a Kokkos view.
template<class SliceType>
Kokkos::View<typename SliceType::value_type*,
             typename SliceType::memory_space>
copySliceToKeys( SliceType slice )
{
    using KeyViewType = Kokkos::View<typename SliceType::value_type*,
                                     typename SliceType::memory_space>;
    KeyViewType keys( "slice_keys", slice.size() );
    Kokkos::RangePolicy<typename SliceType::execution_space>
        exec_policy( 0, slice.size() );
    auto copy_op = KOKKOS_LAMBDA( const std::size_t i ) { keys(i) = slice(i); };
    Kokkos::parallel_for( "Cabana::copySliceToKeys::copy_op",
                          exec_policy,
                          copy_op );
    Kokkos::fence();
    return keys;
}

//---------------------------------------------------------------------------//

} // end namespace Impl

//---------------------------------------------------------------------------//
/*!
  \brief Sort an AoSoA over a subset of its range using a general comparator
  over the given Kokkos View of keys.

  \tparam KeyViewType The Kokkos::View type for keys.

  \tparam Comparator Kokkos::BinSort compatible comparator type.

  \param keys The key values to use for sorting. A key value is needed for
  every element of the AoSoA.

  \param comp The comparator to use for sorting. Must be compatible with
  Kokkos::BinSort.

  \param begin The beginning index of the AoSoA range to sort.

  \param end The end index of the AoSoA range to sort.

  \return The permutation vector associated with the sorting.
*/
template<class KeyViewType, class Comparator>
BinningData<typename KeyViewType::memory_space>
sortByKeyWithComparator( KeyViewType keys,
                         Comparator comp,
                         const std::size_t begin,
                         const std::size_t end,
                         typename std::enable_if<
                         (Kokkos::is_view<KeyViewType>::value),int>::type* = 0 )
{
    auto bin_data = Impl::kokkosBinSort( keys, comp, true, begin, end );
    return bin_data.permuteVector();
}

//---------------------------------------------------------------------------//
/*!
  \brief Sort an entire AoSoA using a general comparator over the given
  Kokkos View of keys.

  \tparam KeyViewType The Kokkos::View type for keys.

  \tparam Comparator Kokkos::BinSort compatible comparator type.

  \param keys The key values to use for sorting. A key value is needed for
  every element of the AoSoA.

  \param comp The comparator to use for sorting. Must be compatible with
  Kokkos::BinSort.

  \return The permutation vector associated with the sorting.
*/
template<class KeyViewType, class Comparator>
BinningData<typename KeyViewType::memory_space>
sortByKeyWithComparator( KeyViewType keys,
                         Comparator comp,
                         typename std::enable_if<
                         (Kokkos::is_view<KeyViewType>::value),int>::type* = 0 )
{
    Impl::kokkosBinSort( keys, comp, true, 0, keys.extent(0) );
}

//---------------------------------------------------------------------------//
/*!
  \brief Bin an AoSoA over a subset of its range using a general comparator
  over the given Kokkos View of keys.

  \tparam KeyViewType The Kokkos::View type for keys.

  \tparam Comparator Kokkos::BinSort compatible comparator type.

  \param keys The key values to use for binning. A key value is needed for
  every element of the AoSoA.

  \param comp The comparator to use for binning. Must be compatible with
  Kokkos::BinSort.

  \param begin The beginning index of the AoSoA range to bin.

  \param end The end index of the AoSoA range to bin.

  \return The binning data (e.g. bin sizes and offsets).
*/
template<class KeyViewType, class Comparator>
BinningData<typename KeyViewType::memory_space>
binByKeyWithComparator( KeyViewType keys,
                        Comparator comp,
                        const std::size_t begin,
                        const std::size_t end,
                        typename std::enable_if<
                        (Kokkos::is_view<KeyViewType>::value),int>::type* = 0 )
{
    return Impl::kokkosBinSort( keys, comp, false, begin, end );
}

//---------------------------------------------------------------------------//
/*!
  \brief Bin an entire AoSoA using a general comparator over the given Kokkos
  View of keys.

  \tparam KeyViewType The Kokkos::View type for keys.

  \tparam Comparator Kokkos::BinSort compatible comparator type.

  \param keys The key values to use for binning. A key value is needed for
  every element of the AoSoA.

  \param comp The comparator to use for binning. Must be compatible with
  Kokkos::BinSort.

  \return The binning data (e.g. bin sizes and offsets).
*/
template<class KeyViewType, class Comparator>
BinningData<typename KeyViewType::memory_space>
binByKeyWithComparator( KeyViewType keys,
                        Comparator comp,
                        typename std::enable_if<
                        (Kokkos::is_view<KeyViewType>::value),int>::type* = 0 )
{
    return Impl::kokkosBinSort( keys, comp, false, 0, keys.extent(0) );
}

//---------------------------------------------------------------------------//
/*!
  \brief Sort an AoSoA over a subset of its range based on the associated key
  values.

  \tparam KeyViewType The Kokkos::View type for keys.

  \param keys The key values to use for sorting. A key value is needed for
  every element of the AoSoA.

  \param begin The beginning index of the AoSoA range to sort.

  \param end The end index of the AoSoA range to sort.

  \return The permutation vector associated with the sorting.
*/
template<class KeyViewType>
BinningData<typename KeyViewType::memory_space>
sortByKey( KeyViewType keys,
           const std::size_t begin,
           const std::size_t end,
           typename std::enable_if<
           (Kokkos::is_view<KeyViewType>::value),int>::type* = 0 )
{
    int nbin = (end - begin) / 2;
    auto bin_data =
        Impl::kokkosBinSort1d( keys, nbin, true, begin, end );
    return bin_data;
}

//---------------------------------------------------------------------------//
/*!
  \brief Sort an entire AoSoA based on the associated key values.

  \tparam KeyViewType The Kokkos::View type for keys.

  \param keys The key values to use for sorting. A key value is needed for
  every element of the AoSoA.

  \return The permutation vector associated with the sorting.

*/
template<class KeyViewType>
BinningData<typename KeyViewType::memory_space>
sortByKey( KeyViewType keys,
           typename std::enable_if<
           (Kokkos::is_view<KeyViewType>::value),int>::type* = 0 )
{
    return sortByKey( keys, 0, keys.extent(0) );
}

//---------------------------------------------------------------------------//
/*!
  \brief Bin an AoSoA over a subset of its range based on the associated key
  values and number of bins. The bins are evenly divided over the range of key
  values.

  \tparam KeyViewType The Kokkos::View type for keys.

  \param keys The key values to use for binning. A key value is needed for
  every element of the AoSoA.

  \param nbin The number of bins to use for binning. The range of key values
  will subdivided equally by the number of bins.

  \param begin The beginning index of the AoSoA range to bin.

  \param end The end index of the AoSoA range to bin.

  \return The binning data (e.g. bin sizes and offsets).
*/
template<class KeyViewType>
BinningData<typename KeyViewType::memory_space>
binByKey( KeyViewType keys,
          const int nbin,
          const std::size_t begin,
          const std::size_t end,
          typename std::enable_if<
          (Kokkos::is_view<KeyViewType>::value),int>::type* = 0 )
{
    return Impl::kokkosBinSort1d( keys, nbin, false, begin, end );
}

//---------------------------------------------------------------------------//
/*!
  \brief Bin an entire AoSoA based on the associated key values and number of
  bins. The bins are evenly divided over the range of key values.

  \tparam KeyViewType The Kokkos::View type for keys.

  \param keys The key values to use for binning. A key value is needed for
  every element of the AoSoA.

  \param nbin The number of bins to use for binning. The range of key values
  will subdivided equally by the number of bins.

  \return The binning data (e.g. bin sizes and offsets).
*/
template<class KeyViewType>
BinningData<typename KeyViewType::memory_space>
binByKey( KeyViewType keys,
          const int nbin,
          typename std::enable_if<
          (Kokkos::is_view<KeyViewType>::value),int>::type* = 0 )
{
    return Impl::kokkosBinSort1d( keys, nbin, false, 0, keys.extent(0) );
}

//---------------------------------------------------------------------------//
/*!
  \brief Sort an AoSoA over a subset of its range based on the associated
  slice of keys.

  \tparam SliceType Slice type for keys.

  \param begin The beginning index of the AoSoA range to sort.

  \param end The end index of the AoSoA range to sort.

  \return The permutation vector associated with the sorting.
*/
template<class SliceType>
BinningData<typename SliceType::memory_space>
sortByKey(
    SliceType slice,
    const std::size_t begin,
    const std::size_t end,
    typename std::enable_if<(is_slice<SliceType>::value),int>::type * = 0 )
{
    auto keys = Impl::copySliceToKeys( slice );
    return sortByKey( keys, begin, end );
}

//---------------------------------------------------------------------------//
/*!
  \brief Sort an entire AoSoA based on the associated slice of keys.

  \tparam SliceType Slice type for keys.

  \param slice Slice of keys.

  \return The permutation vector associated with the sorting.
*/
template<class SliceType>
BinningData<typename SliceType::memory_space>
sortByKey(
    SliceType slice,
    typename std::enable_if<(is_slice<SliceType>::value),int>::type * = 0 )
{
    return sortByKey( slice, 0, slice.size() );
}

//---------------------------------------------------------------------------//
/*!
  \brief Bin an AoSoA over a subset of its range based on the associated
  slice of keys.

  \tparam SliceType Slice type for keys

  \param slice Slice of keys.

  \param nbin The number of bins to use for binning. The range of key values
  will subdivided equally by the number of bins.

  \param begin The beginning index of the AoSoA range to bin.

  \param end The end index of the AoSoA range to bin.

  \return The binning data (e.g. bin sizes and offsets).
*/
template<class SliceType>
BinningData<typename SliceType::memory_space>
binByKey(
    SliceType slice,
    const int nbin,
    const std::size_t begin,
    const std::size_t end,
    typename std::enable_if<(is_slice<SliceType>::value),int>::type * = 0 )
{
    auto keys = Impl::copySliceToKeys( slice );
    return binByKey( keys, nbin, begin, end );
}

//---------------------------------------------------------------------------//
/*!
  \brief Bin an entire AoSoA based on the associated slice of keys.

  \tparam SliceType Slice type for keys.

  \param slice Slice of keys.

  \param nbin The number of bins to use for binning. The range of key values
  will subdivided equally by the number of bins.

  \return The binning data (e.g. bin sizes and offsets).
*/
template<class SliceType>
BinningData<typename SliceType::memory_space>
binByKey(
    SliceType slice,
    const int nbin,
    typename std::enable_if<(is_slice<SliceType>::value),int>::type * = 0 )
{
    return binByKey( slice, nbin, 0, slice.size() );
}

//---------------------------------------------------------------------------//
/*!
  \brief Given binning data permute an AoSoA.

  \tparam BinningDataType The binning data type.

  \tparm AoSoA_t The AoSoA type.

  \param binning_data The binning data.

  \param aosoa The AoSoA to permute.
 */
template<class BinningDataType, class AoSoA_t>
void permute( const BinningDataType& binning_data,
              AoSoA_t& aosoa,
              typename std::enable_if<(is_binning_data<BinningDataType>::value &&
                                       is_aosoa<AoSoA_t>::value),
              int>::type * = 0)
{
    auto begin = binning_data.rangeBegin();
    auto end = binning_data.rangeEnd();

    Kokkos::View<typename AoSoA_t::tuple_type*,
                 typename BinningDataType::memory_space>
        scratch_tuples( "scratch_tuples", end - begin );

    auto permute_to_scratch =
        KOKKOS_LAMBDA( const std::size_t i )
        {
            scratch_tuples( i - begin ) =
                aosoa.getTuple( binning_data.permutation(i-begin) );
        };
    Kokkos::parallel_for(
        "Cabana::kokkosBinSort::permute_to_scratch",
        Kokkos::RangePolicy<typename BinningDataType::execution_space>(begin,end),
        permute_to_scratch );
    Kokkos::fence();

    auto copy_back = KOKKOS_LAMBDA( const std::size_t i )
                     { aosoa.setTuple( i, scratch_tuples(i-begin) ); };
    Kokkos::parallel_for(
        "Cabana::kokkosBinSort::copy_back",
        Kokkos::RangePolicy<typename BinningDataType::execution_space>(begin,end),
        copy_back );
    Kokkos::fence();
}

//---------------------------------------------------------------------------//

} // end namespace Cabana

#endif // end CABANA_SORT_HPP
