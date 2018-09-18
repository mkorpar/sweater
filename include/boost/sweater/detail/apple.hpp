////////////////////////////////////////////////////////////////////////////////
///
/// \file apple.hpp
/// ---------------
///
/// (c) Copyright Domagoj Saric 2016 - 2018.
///
///  Use, modification and distribution are subject to the
///  Boost Software License, Version 1.0. (See accompanying file
///  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
///
///  See http://www.boost.org for most recent version.
///
////////////////////////////////////////////////////////////////////////////////
//------------------------------------------------------------------------------
#pragma once
//------------------------------------------------------------------------------
#include "../hardware_concurrency.hpp"

#include <boost/assert.hpp>
#include <boost/config_ex.hpp>
#include <boost/core/no_exceptions_support.hpp>

#include <algorithm>
#include <cstdint>
#include <future>
#include <thread>
#include <type_traits>

#include <dispatch/dispatch.h>
#include <TargetConditionals.h>
//------------------------------------------------------------------------------
namespace boost
{
//------------------------------------------------------------------------------
namespace sweater
{
//------------------------------------------------------------------------------
namespace apple
{
//------------------------------------------------------------------------------

class shop
{
public:
    using iterations_t = std::uint32_t;

    // http://newosxbook.com/articles/GCD.html
    // http://www.idryman.org/blog/2012/08/05/grand-central-dispatch-vs-openmp
    static auto number_of_workers() noexcept
    {
        BOOST_ASSERT_MSG( hardware_concurrency == std::thread::hardware_concurrency(), "Hardware concurrency changed at runtime!?" );
    #if BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY
        BOOST_ASSUME( hardware_concurrency <= BOOST_SWEATER_MAX_HARDWARE_CONCURRENCY );
    #endif
        return hardware_concurrency;
    }

    template <typename F>
    static void spread_the_sweat( iterations_t const iterations, F && work ) noexcept
    {
        static_assert( noexcept( work( iterations, iterations ) ), "F must be noexcept" );

        /// \note Stride the iteration count based on the number of workers
        /// (otherwise dispatch_apply will make an indirect function call for
        /// each iteration).
        /// The iterations / number_of_workers is an integer division and can
        /// thus be 'lossy' so extra steps need to be taken to account for this.
        ///                                   (04.10.2016.) (Domagoj Saric)
        auto              const number_of_workers    ( shop::number_of_workers() );
        iterations_t      const iterations_per_worker( iterations / number_of_workers );
        std::uint_fast8_t const extra_iterations     ( iterations % number_of_workers );
        auto /*const*/ worker
        (
            [
                &work, iterations_per_worker, extra_iterations
            #ifndef NDEBUG
                , iterations
            #endif // !NDEBUG
            ]
            ( std::uint_fast8_t const worker_index ) noexcept
            {
                auto const extra_iters        ( std::min( worker_index, extra_iterations ) );
                auto const plain_iters        ( worker_index - extra_iters                 );
                auto const this_has_extra_iter( worker_index < extra_iterations            );
                auto const start_iteration
                (
                    extra_iters * ( iterations_per_worker + 1 )
                        +
                    plain_iters *   iterations_per_worker
                );
                auto const stop_iteration( start_iteration + iterations_per_worker + this_has_extra_iter );
                BOOST_ASSERT( stop_iteration <= iterations );
                BOOST_ASSERT_MSG( start_iteration < stop_iteration, "Sweater internal inconsistency: worker called with no work to do." );
                work( start_iteration, stop_iteration );
            }
        );

        /// \note dispatch_apply delegates to dispatch_apply_f so we avoid the
        /// small overhead of an extra jmp and block construction (as opposed to
        /// just a trivial lambda construction).
        ///                                   (04.10.2016.) (Domagoj Saric)
        /// \note The iteration_per_worker logic above does not fully cover the
        /// cases where the number of iterations is less than the number of
        /// workers (resulting in work being called with start_iteration >
        /// stop_iteration) so we have to additionally clamp the iterations
        /// parameter passed to dispatch_apply).
        ///                                   (12.01.2017.) (Domagoj Saric)
        dispatch_apply_f
        (
            std::min<iterations_t>( number_of_workers, iterations ),
            high_priority_queue,
            &worker,
            []( void * const p_context, std::size_t const worker_index ) noexcept
            {
                auto & __restrict the_worker( *static_cast<decltype( worker ) const *>( p_context ) );
                the_worker( static_cast<std::uint_fast8_t>( worker_index ) );
            }
        );
    }

    template <typename F>
    static void fire_and_forget( F && work ) noexcept
    {
        static_assert( noexcept( work() ), "Fire and forget work has to be noexcept" );

        using Functor = std::remove_reference_t<F>;
        if constexpr
        (
            ( sizeof ( work    ) <= sizeof ( void * ) ) &&
            ( alignof( Functor ) <= alignof( void * ) ) &&
            std::is_trivially_copyable    <Functor>::value &&
            std::is_trivially_destructible<Functor>::value
        )
        {
            void * context;
            new ( &context ) Functor( std::forward<F>( work ) );
            dispatch_async_f
            (
                high_priority_queue,
                context,
                []( void * context ) noexcept
                {
                    auto & __restrict the_work( reinterpret_cast<Functor &>( context ) );
                    the_work();
                }
            );
        }
        else
        {
#       if defined( __clang__ )
            /// \note "ObjC++ attempts to copy lambdas, preventing capture of
            /// move-only types". https://llvm.org/bugs/show_bug.cgi?id=20534
            ///                               (14.01.2016.) (Domagoj Saric)
            __block auto moveable_work( std::forward<F>( work ) );
            dispatch_async( high_priority_queue, ^(){ moveable_work(); } );
#       else
            /// \note Still no block support in GCC.
            ///                               (10.06.2017.) (Domagoj Saric)
            auto const p_heap_work( new Functor( std::forward<F>( work ) ) );
            dispatch_async_f
            (
                high_priority_queue,
                p_heap_work,
                []( void * const p_context ) noexcept
                {
                    auto & __restrict the_work( *static_cast<Functor const *>( p_context ) );
                    the_work();
                    delete &the_work;
                }
            );
#       endif // compiler
        }
    }

    template <typename F>
    static auto dispatch( F && work )
    {
        using result_t = typename std::result_of<F()>::type;
        std::promise<result_t> promise;
        std::future<result_t> future( promise.get_future() );
        fire_and_forget
        (
            [promise = std::move( promise ), work = std::forward<F>( work )]
            () mutable noexcept
            {
                BOOST_TRY
                {
                    if constexpr ( std::is_same_v<result_t, void> )
                    {
                        work();
                        promise.set_value();
                    }
                    else
                    {
                        promise.set_value( work() );
                    }
                }
                BOOST_CATCH ( ... )
                {
                    promise.set_exception( std::current_exception() );
                }
                BOOST_CATCH_END
            }
        );
        return future;
    }

private:
    static dispatch_queue_t const default_queue      ;
    static dispatch_queue_t const high_priority_queue;
}; // class shop

__attribute__(( weak )) dispatch_queue_t const shop::default_queue      ( dispatch_get_global_queue( QOS_CLASS_DEFAULT       , 0 ) );
__attribute__(( weak )) dispatch_queue_t const shop::high_priority_queue( dispatch_get_global_queue( QOS_CLASS_USER_INITIATED, 0 ) );

//------------------------------------------------------------------------------
} // namespace apple
//------------------------------------------------------------------------------
} // namespace sweater
//------------------------------------------------------------------------------
} // namespace boost
//------------------------------------------------------------------------------
